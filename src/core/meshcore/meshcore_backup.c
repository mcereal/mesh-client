#include "mesh/core/meshcore_backup.h"

#include "inkwell/base/text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- comparing ----------------------------------------------------------------------------- */

static struct mesh_radio_backup_change *mc_change(struct mesh_radio_backup_diff *diff,
                                                  uint8_t topic, uint16_t index, uint16_t field) {
    return mesh_radio_backup_diff_add(diff, MESH_RADIO_BACKUP_CHANGED, topic, index, field);
}

static void mc_diff_uint(struct mesh_radio_backup_diff *diff, uint8_t topic, uint16_t field,
                         uint64_t a, uint64_t b) {
    if (a == b) {
        return;
    }
    struct mesh_radio_backup_change *change = mc_change(diff, topic, 0U, field);
    if (change != NULL) {
        mesh_radio_backup_value_uint(&change->before, a);
        mesh_radio_backup_value_uint(&change->after, b);
    }
}

static void mc_diff_bool(struct mesh_radio_backup_diff *diff, uint8_t topic, uint16_t field, bool a,
                         bool b) {
    if (a == b) {
        return;
    }
    struct mesh_radio_backup_change *change = mc_change(diff, topic, 0U, field);
    if (change != NULL) {
        mesh_radio_backup_value_bool(&change->before, a);
        mesh_radio_backup_value_bool(&change->after, b);
    }
}

static void mc_diff_degrees(struct mesh_radio_backup_diff *diff, uint16_t field, int32_t a,
                            int32_t b) {
    if (a == b) {
        return;
    }
    struct mesh_radio_backup_change *change =
        mc_change(diff, MESH_RADIO_BACKUP_TOPIC_POSITION, 0U, field);
    if (change != NULL) {
        mesh_radio_backup_value_decimal(&change->before, a, 6U);
        mesh_radio_backup_value_decimal(&change->after, b, 6U);
    }
}

static void mc_diff_self(struct mesh_radio_backup_diff *diff,
                         const struct mesh_meshcore_self_info *a,
                         const struct mesh_meshcore_self_info *b) {
    if (strcmp(a->name, b->name) != 0) {
        struct mesh_radio_backup_change *change =
            mc_change(diff, MESH_RADIO_BACKUP_TOPIC_OWNER, 0U, MESH_MESHCORE_BACKUP_FIELD_NAME);
        if (change != NULL) {
            mesh_radio_backup_value_text(&change->before, a->name, sizeof a->name);
            mesh_radio_backup_value_text(&change->after, b->name, sizeof b->name);
        }
    }
    const uint8_t lora = MESH_RADIO_BACKUP_TOPIC_LORA;
    mc_diff_uint(diff, lora, MESH_MESHCORE_BACKUP_FIELD_FREQUENCY, a->frequency_khz,
                 b->frequency_khz);
    mc_diff_uint(diff, lora, MESH_MESHCORE_BACKUP_FIELD_BANDWIDTH, a->bandwidth_hz,
                 b->bandwidth_hz);
    mc_diff_uint(diff, lora, MESH_MESHCORE_BACKUP_FIELD_SPREADING, a->spreading_factor,
                 b->spreading_factor);
    mc_diff_uint(diff, lora, MESH_MESHCORE_BACKUP_FIELD_CODING, a->coding_rate, b->coding_rate);
    mc_diff_uint(diff, lora, MESH_MESHCORE_BACKUP_FIELD_TX_POWER, a->tx_power_dbm, b->tx_power_dbm);
    mc_diff_degrees(diff, MESH_MESHCORE_BACKUP_FIELD_LATITUDE, a->latitude_e6, b->latitude_e6);
    mc_diff_degrees(diff, MESH_MESHCORE_BACKUP_FIELD_LONGITUDE, a->longitude_e6, b->longitude_e6);
    mc_diff_uint(diff, MESH_RADIO_BACKUP_TOPIC_POSITION, MESH_MESHCORE_BACKUP_FIELD_ADVERT_LOCATION,
                 a->advert_loc_policy, b->advert_loc_policy);
    const uint8_t device = MESH_RADIO_BACKUP_TOPIC_DEVICE;
    mc_diff_bool(diff, device, MESH_MESHCORE_BACKUP_FIELD_MANUAL_ADD, a->manual_add_contacts != 0U,
                 b->manual_add_contacts != 0U);
    mc_diff_uint(diff, device, MESH_MESHCORE_BACKUP_FIELD_TELEMETRY, a->telemetry_modes,
                 b->telemetry_modes);
    mc_diff_uint(diff, device, MESH_MESHCORE_BACKUP_FIELD_MULTI_ACKS, a->multi_acks, b->multi_acks);
    mc_diff_uint(diff, device, MESH_MESHCORE_BACKUP_FIELD_ADV_TYPE, a->adv_type, b->adv_type);
    if (memcmp(a->public_key, b->public_key, sizeof a->public_key) != 0) {
        struct mesh_radio_backup_change *change = mc_change(
            diff, MESH_RADIO_BACKUP_TOPIC_SECURITY, 0U, MESH_MESHCORE_BACKUP_FIELD_PUBLIC_KEY);
        if (change != NULL) {
            mesh_radio_backup_value_opaque(&change->before);
            mesh_radio_backup_value_opaque(&change->after);
        }
    }
}

static void mc_diff_channel(struct mesh_radio_backup_diff *diff, uint16_t slot, bool in_a,
                            const struct mesh_meshcore_channel *a, bool in_b,
                            const struct mesh_meshcore_channel *b) {
    in_a = in_a && mc_channel_used(a);
    in_b = in_b && mc_channel_used(b);
    if (!in_a && !in_b) {
        return;
    }
    if (in_a != in_b) {
        struct mesh_radio_backup_change *change = mesh_radio_backup_diff_add(
            diff, in_a ? MESH_RADIO_BACKUP_REMOVED : MESH_RADIO_BACKUP_ADDED,
            MESH_RADIO_BACKUP_TOPIC_CHANNEL, slot, MESH_MESHCORE_BACKUP_FIELD_CHANNEL_NAME);
        if (change != NULL) {
            const struct mesh_meshcore_channel *there = in_a ? a : b;
            inkwell_str_copy(change->subject, sizeof change->subject, there->name);
            mesh_radio_backup_value_text(in_a ? &change->before : &change->after, there->name,
                                         sizeof there->name);
        }
        return;
    }
    if (strcmp(a->name, b->name) != 0) {
        struct mesh_radio_backup_change *change = mc_change(
            diff, MESH_RADIO_BACKUP_TOPIC_CHANNEL, slot, MESH_MESHCORE_BACKUP_FIELD_CHANNEL_NAME);
        if (change != NULL) {
            inkwell_str_copy(change->subject, sizeof change->subject, b->name);
            mesh_radio_backup_value_text(&change->before, a->name, sizeof a->name);
            mesh_radio_backup_value_text(&change->after, b->name, sizeof b->name);
        }
    }
    if (memcmp(a->secret, b->secret, sizeof a->secret) != 0) {
        struct mesh_radio_backup_change *change = mc_change(
            diff, MESH_RADIO_BACKUP_TOPIC_CHANNEL, slot, MESH_MESHCORE_BACKUP_FIELD_CHANNEL_SECRET);
        if (change != NULL) {
            inkwell_str_copy(change->subject, sizeof change->subject, b->name);
            mesh_radio_backup_value_opaque(&change->before);
            mesh_radio_backup_value_opaque(&change->after);
        }
    }
}

static const struct mesh_meshcore_contact *
mc_find_contact(const struct mesh_meshcore_backup_contents *contents, const uint8_t *key) {
    for (size_t i = 0; i < contents->contact_count; ++i) {
        if (memcmp(contents->contacts[i].public_key, key, MESH_MESHCORE_PUBKEY_LEN) == 0) {
            return &contents->contacts[i];
        }
    }
    return NULL;
}

static struct mesh_radio_backup_change *mc_contact_change(struct mesh_radio_backup_diff *diff,
                                                          uint8_t kind, uint16_t field,
                                                          const struct mesh_meshcore_contact *c) {
    struct mesh_radio_backup_change *change =
        mesh_radio_backup_diff_add(diff, kind, MESH_RADIO_BACKUP_TOPIC_CONTACT, 0U, field);
    if (change != NULL) {
        inkwell_str_copy(change->subject, sizeof change->subject, c->name);
    }
    return change;
}

static void mc_diff_contacts(struct mesh_radio_backup_diff *diff,
                             const struct mesh_meshcore_backup_contents *a,
                             const struct mesh_meshcore_backup_contents *b) {
    for (size_t i = 0; i < a->contact_count; ++i) {
        const struct mesh_meshcore_contact *was = &a->contacts[i];
        const struct mesh_meshcore_contact *now = mc_find_contact(b, was->public_key);
        if (now == NULL) {
            struct mesh_radio_backup_change *change =
                mc_contact_change(diff, MESH_RADIO_BACKUP_REMOVED, 0U, was);
            if (change != NULL) {
                mesh_radio_backup_value_text(&change->before, was->name, sizeof was->name);
            }
            continue;
        }
        if (strcmp(was->name, now->name) != 0) {
            struct mesh_radio_backup_change *change = mc_contact_change(
                diff, MESH_RADIO_BACKUP_CHANGED, MESH_MESHCORE_BACKUP_FIELD_NAME, now);
            if (change != NULL) {
                mesh_radio_backup_value_text(&change->before, was->name, sizeof was->name);
                mesh_radio_backup_value_text(&change->after, now->name, sizeof now->name);
            }
        }
        if (was->type != now->type) {
            struct mesh_radio_backup_change *change = mc_contact_change(
                diff, MESH_RADIO_BACKUP_CHANGED, MESH_MESHCORE_BACKUP_FIELD_CONTACT_TYPE, now);
            if (change != NULL) {
                mesh_radio_backup_value_uint(&change->before, was->type);
                mesh_radio_backup_value_uint(&change->after, now->type);
            }
        }
        if (was->flags != now->flags) {
            struct mesh_radio_backup_change *change = mc_contact_change(
                diff, MESH_RADIO_BACKUP_CHANGED, MESH_MESHCORE_BACKUP_FIELD_CONTACT_FLAGS, now);
            if (change != NULL) {
                mesh_radio_backup_value_uint(&change->before, was->flags);
                mesh_radio_backup_value_uint(&change->after, now->flags);
            }
        }
    }
    for (size_t i = 0; i < b->contact_count; ++i) {
        const struct mesh_meshcore_contact *now = &b->contacts[i];
        if (mc_find_contact(a, now->public_key) == NULL) {
            struct mesh_radio_backup_change *change =
                mc_contact_change(diff, MESH_RADIO_BACKUP_ADDED, 0U, now);
            if (change != NULL) {
                mesh_radio_backup_value_text(&change->after, now->name, sizeof now->name);
            }
        }
    }
}

int mesh_meshcore_backup_diff(const struct mesh_radio_backup *a, const struct mesh_radio_backup *b,
                              struct mesh_radio_backup_diff *out) {
    if (a == NULL || b == NULL || out == NULL) {
        return -EINVAL;
    }
    mesh_radio_backup_diff_reset(out, MESH_RADIO_BACKUP_MESHCORE);
    if (a->header.protocol != MESH_RADIO_BACKUP_MESHCORE ||
        b->header.protocol != MESH_RADIO_BACKUP_MESHCORE) {
        return -EPROTO;
    }
    struct mesh_meshcore_backup_contents *contents = malloc(2U * sizeof *contents);
    if (contents == NULL) {
        return -ENOMEM;
    }
    struct mesh_meshcore_backup_contents *was = &contents[0];
    struct mesh_meshcore_backup_contents *now = &contents[1];
    int result = mesh_meshcore_backup_read(a, was);
    if (result == 0) {
        result = mesh_meshcore_backup_read(b, now);
    }
    if (result == 0) {
        if (was->has_self && now->has_self) {
            mc_diff_self(out, &was->self, &now->self);
        }
        if (was->has_pin != now->has_pin || was->ble_pin != now->ble_pin) {
            struct mesh_radio_backup_change *change = mc_change(
                out, MESH_RADIO_BACKUP_TOPIC_BLUETOOTH, 0U, MESH_MESHCORE_BACKUP_FIELD_PIN);
            if (change != NULL) {
                if (was->has_pin) {
                    mesh_radio_backup_value_uint(&change->before, was->ble_pin);
                }
                if (now->has_pin) {
                    mesh_radio_backup_value_uint(&change->after, now->ble_pin);
                }
            }
        }
        for (uint16_t slot = 0U; slot < MESH_MESHCORE_CHANNELS_KEPT; ++slot) {
            mc_diff_channel(out, slot, was->has_channel[slot], &was->channels[slot],
                            now->has_channel[slot], &now->channels[slot]);
        }
        mc_diff_contacts(out, was, now);
    }
    free(contents);
    return result;
}

/* ---- restoring ----------------------------------------------------------------------------- */

static bool mc_channel_same(const struct mesh_meshcore_channel *a, bool in_b,
                            const struct mesh_meshcore_channel *b) {
    const bool in_a = mc_channel_used(a);
    in_b = in_b && mc_channel_used(b);
    if (!in_a || !in_b) {
        return in_a == in_b;
    }
    return strcmp(a->name, b->name) == 0 && memcmp(a->secret, b->secret, sizeof a->secret) == 0;
}

/* The contacts the radio and the backup disagree about, which this restore does not write. */
static size_t mc_contacts_differing(const struct mesh_meshcore_backup_contents *saved,
                                    const struct mesh_meshcore *meshcore) {
    size_t differing = 0U;
    for (size_t i = 0; i < saved->contact_count; ++i) {
        const struct mesh_meshcore_contact *want = &saved->contacts[i];
        const struct mesh_meshcore_contact *have = NULL;
        for (size_t k = 0; k < meshcore->contact_count && have == NULL; ++k) {
            if (memcmp(meshcore->contacts[k].public_key, want->public_key,
                       MESH_MESHCORE_PUBKEY_LEN) == 0) {
                have = &meshcore->contacts[k];
            }
        }
        if (have == NULL || strcmp(have->name, want->name) != 0 || have->type != want->type ||
            have->flags != want->flags) {
            ++differing;
        }
    }
    for (size_t k = 0; k < meshcore->contact_count; ++k) {
        bool kept = false;
        for (size_t i = 0; i < saved->contact_count && !kept; ++i) {
            kept = memcmp(meshcore->contacts[k].public_key, saved->contacts[i].public_key,
                          MESH_MESHCORE_PUBKEY_LEN) == 0;
        }
        differing += kept ? 0U : 1U;
    }
    return differing;
}

/* The settings groups that differ, into `write`; each one that cannot be written is counted. */
static void mc_plan_self(const struct mesh_meshcore_backup_contents *saved,
                         const struct mesh_meshcore *meshcore,
                         struct mesh_meshcore_settings_write *write, size_t *unwritable) {
    const struct mesh_meshcore_self_info *want = &saved->self;
    const struct mesh_meshcore_self_info *have = &meshcore->self;
    if (strcmp(want->name, have->name) != 0) {
        write->set_name = true;
        inkwell_str_copy(write->name, sizeof write->name, want->name);
    }
    if (want->frequency_khz != have->frequency_khz || want->bandwidth_hz != have->bandwidth_hz ||
        want->spreading_factor != have->spreading_factor ||
        want->coding_rate != have->coding_rate) {
        if (mesh_meshcore_radio_params_valid(want->frequency_khz, want->bandwidth_hz,
                                             want->spreading_factor, want->coding_rate)) {
            write->set_radio = true;
            write->frequency_khz = want->frequency_khz;
            write->bandwidth_hz = want->bandwidth_hz;
            write->spreading_factor = want->spreading_factor;
            write->coding_rate = want->coding_rate;
        } else {
            ++*unwritable;
        }
    }
    if (want->tx_power_dbm != have->tx_power_dbm) {
        const int8_t dbm = (int8_t)want->tx_power_dbm;
        if (mesh_meshcore_tx_power_valid(meshcore, dbm)) {
            write->set_tx_power = true;
            write->tx_power_dbm = dbm;
        } else {
            ++*unwritable;
        }
    }
    if (want->latitude_e6 != have->latitude_e6 || want->longitude_e6 != have->longitude_e6) {
        write->set_position = true;
        write->latitude_e6 = want->latitude_e6;
        write->longitude_e6 = want->longitude_e6;
    }
    if (want->manual_add_contacts != have->manual_add_contacts ||
        want->telemetry_modes != have->telemetry_modes ||
        want->advert_loc_policy != have->advert_loc_policy ||
        want->multi_acks != have->multi_acks) {
        write->set_other = true;
        write->manual_add_contacts = want->manual_add_contacts;
        write->telemetry_modes = want->telemetry_modes;
        write->advert_loc_policy = want->advert_loc_policy;
        write->multi_acks = want->multi_acks;
    }
    if (want->adv_type != have->adv_type) {
        ++*unwritable; /* the companion firmware says what it is; no command changes it */
    }
    const uint32_t pin = meshcore->has_device ? meshcore->device.ble_pin : 0U;
    if (saved->has_pin && saved->ble_pin != pin) {
        if (saved->ble_pin == 0U || (saved->ble_pin >= 100000U && saved->ble_pin <= 999999U)) {
            write->set_pin = true;
            write->ble_pin = saved->ble_pin;
        } else {
            ++*unwritable;
        }
    }
}

int mesh_meshcore_backup_plan(const struct mesh_radio_backup *backup,
                              const struct mesh_meshcore *meshcore,
                              struct mesh_meshcore_settings_write *writes, size_t max,
                              size_t *unwritable) {
    if (backup == NULL || meshcore == NULL || writes == NULL || unwritable == NULL) {
        return -EINVAL;
    }
    *unwritable = 0U;
    if (backup->header.protocol != MESH_RADIO_BACKUP_MESHCORE) {
        return -EPROTO;
    }
    if (!mesh_meshcore_backup_ready(meshcore)) {
        return -EAGAIN;
    }
    struct mesh_meshcore_backup_contents *saved = malloc(sizeof *saved);
    if (saved == NULL) {
        return -ENOMEM;
    }
    int result = mesh_meshcore_backup_read(backup, saved);
    /* The key is the radio: a backup of another one is not restored onto this one, whatever
       its node number says. */
    if (result == 0 && saved->has_self &&
        memcmp(saved->self.public_key, meshcore->self.public_key, MESH_MESHCORE_PUBKEY_LEN) != 0) {
        result = -ENODEV;
    }
    struct mesh_meshcore_settings_write planned[MESH_MESHCORE_BACKUP_PLAN_MAX];
    size_t count = 0U;
    size_t skipped = 0U;
    if (result == 0) {
        memset(planned, 0, sizeof planned);
        struct mesh_meshcore_settings_write *self = &planned[0];
        if (saved->has_self) {
            mc_plan_self(saved, meshcore, self, &skipped);
        }
        if (self->set_name || self->set_radio || self->set_tx_power || self->set_position ||
            self->set_other || self->set_pin) {
            count = 1U;
        }
        for (uint8_t slot = 0U; slot < MESH_MESHCORE_CHANNELS_KEPT; ++slot) {
            if (!saved->has_channel[slot] ||
                mc_channel_same(&saved->channels[slot], meshcore->has_channel[slot],
                                &meshcore->channels[slot])) {
                continue;
            }
            /* A slot the walk did not read is one this radio does not have. */
            if (!meshcore->has_channel[slot]) {
                ++skipped;
                continue;
            }
            struct mesh_meshcore_settings_write *write = &planned[count++];
            write->set_channel = true;
            write->channel_index = slot;
            /* An unused slot restored is a slot cleared: an empty name and a zero secret. */
            memcpy(write->channel_name, saved->channels[slot].name, MESH_MESHCORE_NAME_LEN);
            memcpy(write->channel_secret, saved->channels[slot].secret, MESH_MESHCORE_SECRET_LEN);
        }
        skipped += mc_contacts_differing(saved, meshcore);
        if (count > max) {
            result = -ENOSPC;
        }
    }
    free(saved);
    if (result != 0) {
        return result;
    }
    memcpy(writes, planned, count * sizeof *writes);
    *unwritable = skipped;
    return (int)count;
}
