#include "mesh/core/radio_backup_meshtastic.h"

#include <pb_common.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every Config section a backup must hold, in the order they are written. */
static const pb_size_t k_config_tags[] = {
    meshtastic_Config_device_tag,    meshtastic_Config_position_tag, meshtastic_Config_power_tag,
    meshtastic_Config_network_tag,   meshtastic_Config_display_tag,  meshtastic_Config_lora_tag,
    meshtastic_Config_bluetooth_tag, meshtastic_Config_security_tag,
};

#define MT_COUNT(table) (sizeof(table) / sizeof((table)[0]))

/* The kept section for a Config tag, as a Config with that variant set; false when not held. */
static bool mt_config_load(const struct mesh_radio_settings *settings, pb_size_t tag,
                           meshtastic_Config *out) {
    memset(out, 0, sizeof *out);
    out->which_payload_variant = tag;
    switch (tag) {
    case meshtastic_Config_device_tag:
        out->payload_variant.device = settings->device;
        return settings->has_device;
    case meshtastic_Config_position_tag:
        out->payload_variant.position = settings->position;
        return settings->has_position;
    case meshtastic_Config_power_tag:
        out->payload_variant.power = settings->power;
        return settings->has_power;
    case meshtastic_Config_network_tag:
        out->payload_variant.network = settings->network;
        return settings->has_network;
    case meshtastic_Config_display_tag:
        out->payload_variant.display = settings->display;
        return settings->has_display;
    case meshtastic_Config_lora_tag:
        out->payload_variant.lora = settings->lora;
        return settings->has_lora;
    case meshtastic_Config_bluetooth_tag:
        out->payload_variant.bluetooth = settings->bluetooth;
        return settings->has_bluetooth;
    case meshtastic_Config_security_tag:
        out->payload_variant.security = settings->security;
        /* The one field that never goes on the card; see the header. */
        memset(&out->payload_variant.security.private_key, 0,
               sizeof out->payload_variant.security.private_key);
        return settings->has_security;
    default:
        return false;
    }
}

bool mesh_radio_backup_meshtastic_ready(const struct mesh_radio_settings *settings) {
    if (settings == NULL || settings->admin_dest != 0U || !settings->has_owner ||
        !settings->has_metadata) {
        return false;
    }
    meshtastic_Config config;
    for (size_t i = 0; i < MT_COUNT(k_config_tags); ++i) {
        if (!mt_config_load(settings, k_config_tags[i], &config)) {
            return false;
        }
    }
    for (size_t i = 0; i < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++i) {
        if (!settings->has_channel[i]) {
            return false;
        }
    }
    return true;
}

/* Encodes `message` and appends it under `tag`. */
static int mt_add(struct mesh_radio_backup *backup, uint16_t tag, const pb_msgdesc_t *fields,
                  const void *message) {
    uint8_t buffer[MESH_RADIO_BACKUP_SECTION_MAX];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof buffer);
    if (!pb_encode(&stream, fields, message)) {
        return -EINVAL;
    }
    /* Possibly nothing at all - a disabled slot 0 is every default - which is still a section. */
    return mesh_radio_backup_add(backup, tag, buffer, stream.bytes_written);
}

/* Text sections keep their terminator, so an empty list and a missing one stay different. */
static int mt_add_text(struct mesh_radio_backup *backup, uint16_t tag, const char *text) {
    return mesh_radio_backup_add(backup, tag, text, strlen(text) + 1U);
}

/* Bandwidth as the header wants it, in Hz: the two narrow ones are written as whole kHz. */
static uint32_t mt_bandwidth_hz(uint32_t khz) {
    if (khz == 31U) {
        return 31250U;
    }
    if (khz == 62U) {
        return 62500U;
    }
    return khz * 1000U;
}

static void mt_header(const struct mesh_radio_settings *settings,
                      const struct mesh_handshake_status *status,
                      struct mesh_radio_backup_header *header) {
    header->protocol = MESH_RADIO_BACKUP_MESHTASTIC;
    header->node_id = status->my_info.my_node_num;
    snprintf(header->name, sizeof header->name, "%s", settings->owner.long_name);
    char model[32];
    snprintf(header->model, sizeof header->model, "%s",
             mesh_radio_hw_model_name(settings->metadata.hw_model, model, sizeof model));
    snprintf(header->firmware, sizeof header->firmware, "%s", settings->metadata.firmware_version);

    const meshtastic_Config_LoRaConfig *lora = &settings->lora;
    snprintf(header->region, sizeof header->region, "%s", mesh_radio_region_name(lora->region));
    header->has_radio = true;
    header->frequency_khz = lora->override_frequency > 0.0f
                                ? (uint32_t)(lora->override_frequency * 1000.0f + 0.5f)
                                : 0U;
    header->tx_power_dbm = (int8_t)lora->tx_power;
    if (lora->use_preset) {
        snprintf(header->preset, sizeof header->preset, "%s",
                 mesh_radio_modem_preset_name(lora->modem_preset));
    } else {
        header->bandwidth_hz = mt_bandwidth_hz(lora->bandwidth);
        header->spreading_factor = (uint8_t)lora->spread_factor;
        header->coding_rate = (uint8_t)lora->coding_rate;
    }

    for (size_t i = 0; i < MESH_RADIO_SETTINGS_MAX_CHANNELS && i < MESH_RADIO_BACKUP_CHANNELS;
         ++i) {
        const meshtastic_Channel *channel = &settings->channels[i];
        if (channel->role == meshtastic_Channel_Role_DISABLED) {
            continue;
        }
        snprintf(header->channel_names[i], sizeof header->channel_names[i], "%s",
                 channel->has_settings ? channel->settings.name : "");
        header->channel_count = (uint8_t)(i + 1U);
    }

    header->has_nodes_heard = true;
    header->nodes_heard = status->my_info.nodedb_count;
}

/* Our own fix, when the radio is told to use one it was given rather than one it finds. */
static bool mt_fixed_position(const struct mesh_radio_settings *settings,
                              const struct mesh_handshake_status *status,
                              meshtastic_Position *out) {
    if (!settings->position.fixed_position) {
        return false;
    }
    for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        const struct mesh_node_summary *node = &status->nodes[i];
        if (node->node_id != status->my_info.my_node_num || !node->position.valid) {
            continue;
        }
        memset(out, 0, sizeof *out);
        out->has_latitude_i = true;
        out->latitude_i = node->position.latitude_i;
        out->has_longitude_i = true;
        out->longitude_i = node->position.longitude_i;
        if (node->position.has_altitude) {
            out->has_altitude = true;
            out->altitude = node->position.altitude;
        }
        return true;
    }
    return false;
}

int mesh_radio_backup_meshtastic_capture(const struct mesh_radio_settings *settings,
                                         const struct mesh_handshake_status *status,
                                         struct mesh_radio_backup *backup) {
    if (backup == NULL) {
        return -EINVAL;
    }
    mesh_radio_backup_reset(backup);
    if (settings == NULL || status == NULL || !status->has_my_info ||
        status->my_info.my_node_num == 0U || !mesh_radio_backup_meshtastic_ready(settings)) {
        return -EAGAIN;
    }
    mt_header(settings, status, &backup->header);

    int result = 0;
    meshtastic_Config config;
    for (size_t i = 0; result == 0 && i < MT_COUNT(k_config_tags); ++i) {
        (void)mt_config_load(settings, k_config_tags[i], &config);
        result = mt_add(backup, MESH_RADIO_BACKUP_MT_CONFIG, meshtastic_Config_fields, &config);
    }
    meshtastic_ModuleConfig module;
    for (size_t i = 0; result == 0 && i < mesh_radio_module_count(); ++i) {
        const struct mesh_module_binding *binding = mesh_radio_module_at(i);
        if (binding != NULL && mesh_radio_module_load(settings, binding, &module)) {
            result = mt_add(backup, MESH_RADIO_BACKUP_MT_MODULE, meshtastic_ModuleConfig_fields,
                            &module);
        }
    }
    for (size_t i = 0; result == 0 && i < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++i) {
        result = mt_add(backup, MESH_RADIO_BACKUP_MT_CHANNEL, meshtastic_Channel_fields,
                        &settings->channels[i]);
    }
    if (result == 0) {
        result =
            mt_add(backup, MESH_RADIO_BACKUP_MT_OWNER, meshtastic_User_fields, &settings->owner);
    }
    if (result == 0 && settings->has_ui_config) {
        result = mt_add(backup, MESH_RADIO_BACKUP_MT_UI_CONFIG, meshtastic_DeviceUIConfig_fields,
                        &settings->ui_config);
    }
    if (result == 0 && settings->has_canned_messages) {
        result = mt_add_text(backup, MESH_RADIO_BACKUP_MT_CANNED, settings->canned_messages);
    }
    if (result == 0 && settings->has_ringtone) {
        result = mt_add_text(backup, MESH_RADIO_BACKUP_MT_RINGTONE, settings->ringtone);
    }
    meshtastic_Position position;
    if (result == 0 && mt_fixed_position(settings, status, &position)) {
        result =
            mt_add(backup, MESH_RADIO_BACKUP_MT_POSITION, meshtastic_Position_fields, &position);
    }
    if (result != 0) {
        mesh_radio_backup_reset(backup);
    }
    return result;
}

/* ---- reading back -------------------------------------------------------------------------- */

static bool mt_decode(const uint8_t *data, size_t len, const pb_msgdesc_t *fields, void *out) {
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    return pb_decode(&stream, fields, out);
}

/* A text section into a fixed field: it must fit and end in its terminator. */
static bool mt_text(const uint8_t *data, size_t len, char *out, size_t out_len) {
    if (len == 0U || len > out_len || data[len - 1U] != '\0') {
        return false;
    }
    memcpy(out, data, len);
    return true;
}

int mesh_radio_backup_meshtastic_read(const struct mesh_radio_backup *backup,
                                      struct mesh_radio_settings *settings,
                                      meshtastic_Position *position) {
    if (backup == NULL || settings == NULL) {
        return -EINVAL;
    }
    if (backup->header.protocol != MESH_RADIO_BACKUP_MESHTASTIC) {
        return -EPROTO;
    }
    mesh_radio_settings_reset(settings);
    if (position != NULL) {
        memset(position, 0, sizeof *position);
    }
    const uint8_t *data = NULL;
    const struct mesh_radio_backup_section *section = NULL;
    for (size_t i = 0; (section = mesh_radio_backup_section_at(backup, i, &data)) != NULL; ++i) {
        bool ok = true;
        switch (section->tag) {
        case MESH_RADIO_BACKUP_MT_CONFIG: {
            meshtastic_Config config = meshtastic_Config_init_zero;
            ok = mt_decode(data, section->len, meshtastic_Config_fields, &config);
            if (ok) {
                mesh_radio_settings_apply_config(settings, &config);
            }
            break;
        }
        case MESH_RADIO_BACKUP_MT_MODULE: {
            meshtastic_ModuleConfig module = meshtastic_ModuleConfig_init_zero;
            ok = mt_decode(data, section->len, meshtastic_ModuleConfig_fields, &module);
            if (ok) {
                mesh_radio_settings_apply_module_config(settings, &module);
            }
            break;
        }
        case MESH_RADIO_BACKUP_MT_CHANNEL: {
            meshtastic_Channel channel = meshtastic_Channel_init_zero;
            ok = mt_decode(data, section->len, meshtastic_Channel_fields, &channel) &&
                 channel.index >= 0 && (size_t)channel.index < MESH_RADIO_SETTINGS_MAX_CHANNELS;
            if (ok) {
                mesh_radio_settings_apply_channel(settings, &channel);
            }
            break;
        }
        case MESH_RADIO_BACKUP_MT_OWNER: {
            meshtastic_User owner = meshtastic_User_init_zero;
            ok = mt_decode(data, section->len, meshtastic_User_fields, &owner);
            if (ok) {
                mesh_radio_settings_apply_owner(settings, &owner);
            }
            break;
        }
        case MESH_RADIO_BACKUP_MT_UI_CONFIG: {
            meshtastic_DeviceUIConfig ui = meshtastic_DeviceUIConfig_init_zero;
            ok = mt_decode(data, section->len, meshtastic_DeviceUIConfig_fields, &ui);
            if (ok) {
                mesh_radio_settings_apply_ui_config(settings, &ui);
            }
            break;
        }
        case MESH_RADIO_BACKUP_MT_CANNED:
            ok = mt_text(data, section->len, settings->canned_messages,
                         sizeof settings->canned_messages);
            settings->has_canned_messages = ok;
            break;
        case MESH_RADIO_BACKUP_MT_RINGTONE:
            ok = mt_text(data, section->len, settings->ringtone, sizeof settings->ringtone);
            settings->has_ringtone = ok;
            break;
        case MESH_RADIO_BACKUP_MT_POSITION:
            if (position != NULL) {
                ok = mt_decode(data, section->len, meshtastic_Position_fields, position);
            }
            break;
        default:
            break; /* a section a later build added */
        }
        if (!ok) {
            mesh_radio_settings_reset(settings);
            return -EBADMSG;
        }
    }
    return 0;
}

/* ---- comparing ----------------------------------------------------------------------------- */

/*
 * Every section decodes into one of these; the largest decides the size, and the walk below
 * reads them only through nanopb's field iterator, so which member was written does not matter
 * to it.
 */
union mt_message {
    meshtastic_Config config;
    meshtastic_ModuleConfig module;
    meshtastic_Channel channel;
    meshtastic_User owner;
    meshtastic_DeviceUIConfig ui;
    meshtastic_Position position;
};

/* What a section is about: its topic, and which one of it - a Config variant, a module, a slot. */
struct mt_key {
    uint16_t tag;
    uint8_t topic;
    uint16_t index;
    bool ok;
};

static const pb_msgdesc_t *mt_fields(uint16_t tag) {
    switch (tag) {
    case MESH_RADIO_BACKUP_MT_CONFIG:
        return meshtastic_Config_fields;
    case MESH_RADIO_BACKUP_MT_MODULE:
        return meshtastic_ModuleConfig_fields;
    case MESH_RADIO_BACKUP_MT_CHANNEL:
        return meshtastic_Channel_fields;
    case MESH_RADIO_BACKUP_MT_OWNER:
        return meshtastic_User_fields;
    case MESH_RADIO_BACKUP_MT_UI_CONFIG:
        return meshtastic_DeviceUIConfig_fields;
    case MESH_RADIO_BACKUP_MT_POSITION:
        return meshtastic_Position_fields;
    default:
        return NULL;
    }
}

static uint8_t mt_config_topic(pb_size_t variant) {
    switch (variant) {
    case meshtastic_Config_device_tag:
        return MESH_RADIO_BACKUP_TOPIC_DEVICE;
    case meshtastic_Config_position_tag:
        return MESH_RADIO_BACKUP_TOPIC_POSITION;
    case meshtastic_Config_power_tag:
        return MESH_RADIO_BACKUP_TOPIC_POWER;
    case meshtastic_Config_network_tag:
        return MESH_RADIO_BACKUP_TOPIC_NETWORK;
    case meshtastic_Config_display_tag:
        return MESH_RADIO_BACKUP_TOPIC_DISPLAY;
    case meshtastic_Config_lora_tag:
        return MESH_RADIO_BACKUP_TOPIC_LORA;
    case meshtastic_Config_bluetooth_tag:
        return MESH_RADIO_BACKUP_TOPIC_BLUETOOTH;
    case meshtastic_Config_security_tag:
        return MESH_RADIO_BACKUP_TOPIC_SECURITY;
    default:
        return MESH_RADIO_BACKUP_TOPIC_NONE;
    }
}

/* The oneof member a Config or a ModuleConfig carries: its descriptor and where it sits. */
static bool mt_variant(const pb_msgdesc_t *fields, const void *message,
                       const pb_msgdesc_t **variant_fields, const void **variant, pb_size_t *tag) {
    pb_field_iter_t iter;
    if (!pb_field_iter_begin_const(&iter, fields, message)) {
        return false;
    }
    do {
        if (PB_HTYPE(iter.type) == PB_HTYPE_ONEOF && PB_LTYPE_IS_SUBMSG(iter.type) &&
            iter.pSize != NULL && *(const pb_size_t *)iter.pSize == iter.tag) {
            *variant_fields = iter.submsg_desc;
            *variant = iter.pData;
            *tag = iter.tag;
            return true;
        }
    } while (pb_field_iter_next(&iter));
    return false;
}

static struct mt_key mt_key_of(uint16_t tag, const uint8_t *data, size_t len,
                               union mt_message *message) {
    struct mt_key key = {.tag = tag, .ok = true};
    memset(message, 0, sizeof *message);
    const pb_msgdesc_t *fields = mt_fields(tag);
    if (fields != NULL && !mt_decode(data, len, fields, message)) {
        key.ok = false;
        return key;
    }
    switch (tag) {
    case MESH_RADIO_BACKUP_MT_CONFIG:
        key.index = message->config.which_payload_variant;
        key.topic = mt_config_topic(message->config.which_payload_variant);
        break;
    case MESH_RADIO_BACKUP_MT_MODULE:
        key.index = message->module.which_payload_variant;
        key.topic = MESH_RADIO_BACKUP_TOPIC_MODULE;
        break;
    case MESH_RADIO_BACKUP_MT_CHANNEL:
        key.index = (uint16_t)message->channel.index;
        key.topic = MESH_RADIO_BACKUP_TOPIC_CHANNEL;
        break;
    case MESH_RADIO_BACKUP_MT_OWNER:
        key.topic = MESH_RADIO_BACKUP_TOPIC_OWNER;
        break;
    case MESH_RADIO_BACKUP_MT_UI_CONFIG:
        key.topic = MESH_RADIO_BACKUP_TOPIC_RADIO_UI;
        break;
    case MESH_RADIO_BACKUP_MT_CANNED:
        key.topic = MESH_RADIO_BACKUP_TOPIC_CANNED;
        break;
    case MESH_RADIO_BACKUP_MT_RINGTONE:
        key.topic = MESH_RADIO_BACKUP_TOPIC_RINGTONE;
        break;
    case MESH_RADIO_BACKUP_MT_POSITION:
        key.topic = MESH_RADIO_BACKUP_TOPIC_FIXED_POSITION;
        break;
    default:
        key.ok = false; /* a section a later build added: nothing here can say what it is */
        break;
    }
    return key;
}

static int64_t mt_signed(const void *p, size_t size) {
    switch (size) {
    case 1:
        return *(const int8_t *)p;
    case 2:
        return *(const int16_t *)p;
    case 8:
        return *(const int64_t *)p;
    default:
        return *(const int32_t *)p;
    }
}

static uint64_t mt_unsigned(const void *p, size_t size) {
    switch (size) {
    case 1:
        return *(const uint8_t *)p;
    case 2:
        return *(const uint16_t *)p;
    case 8:
        return *(const uint64_t *)p;
    default:
        return *(const uint32_t *)p;
    }
}

/* Whether a field is set: a has_ flag, a oneof naming it, a repeated one with anything in it. */
static bool mt_present(const pb_field_iter_t *iter) {
    switch (PB_HTYPE(iter->type)) {
    case PB_HTYPE_OPTIONAL:
        return iter->pSize == NULL || *(const bool *)iter->pSize;
    case PB_HTYPE_ONEOF:
        return *(const pb_size_t *)iter->pSize == iter->tag;
    case PB_HTYPE_REPEATED:
        return iter->pSize != &iter->array_size ? *(const pb_size_t *)iter->pSize > 0U : true;
    default:
        return true;
    }
}

/* A field's value as a line can show it; a list, a key or a message is only "set". */
static void mt_value(const pb_field_iter_t *iter, struct mesh_radio_backup_value *value) {
    if (PB_HTYPE(iter->type) == PB_HTYPE_REPEATED) {
        mesh_radio_backup_value_opaque(value);
        return;
    }
    switch (PB_LTYPE(iter->type)) {
    case PB_LTYPE_BOOL:
        mesh_radio_backup_value_bool(value, *(const bool *)iter->pData);
        break;
    case PB_LTYPE_VARINT:
    case PB_LTYPE_SVARINT:
        mesh_radio_backup_value_int(value, mt_signed(iter->pData, iter->data_size));
        break;
    case PB_LTYPE_UVARINT:
    case PB_LTYPE_FIXED32:
    case PB_LTYPE_FIXED64:
        mesh_radio_backup_value_uint(value, mt_unsigned(iter->pData, iter->data_size));
        break;
    case PB_LTYPE_STRING:
        mesh_radio_backup_value_text(value, iter->pData, iter->data_size);
        break;
    default:
        mesh_radio_backup_value_opaque(value);
        break;
    }
}

/* Whether two present fields hold the same value, compared the way the type is stored. */
static bool mt_same(const pb_field_iter_t *a, const pb_field_iter_t *b) {
    if (PB_HTYPE(a->type) == PB_HTYPE_REPEATED) {
        const pb_size_t count = a->pSize != NULL ? *(const pb_size_t *)a->pSize : a->array_size;
        const pb_size_t other = b->pSize != NULL ? *(const pb_size_t *)b->pSize : b->array_size;
        return count == other && memcmp(a->pData, b->pData, (size_t)count * a->data_size) == 0;
    }
    switch (PB_LTYPE(a->type)) {
    case PB_LTYPE_BOOL:
        return *(const bool *)a->pData == *(const bool *)b->pData;
    case PB_LTYPE_STRING:
        return strncmp(a->pData, b->pData, a->data_size) == 0;
    case PB_LTYPE_BYTES: {
        const pb_bytes_array_t *x = a->pData;
        const pb_bytes_array_t *y = b->pData;
        return x->size == y->size && memcmp(x->bytes, y->bytes, x->size) == 0;
    }
    default:
        return memcmp(a->pData, b->pData, a->data_size) == 0;
    }
}

static void mt_walk(struct mesh_radio_backup_diff *diff, const struct mt_key *key, uint16_t parent,
                    unsigned depth, const pb_msgdesc_t *fields, const void *a, const void *b) {
    pb_field_iter_t ia;
    pb_field_iter_t ib;
    if (!pb_field_iter_begin_const(&ia, fields, a) || !pb_field_iter_begin_const(&ib, fields, b)) {
        return;
    }
    do {
        if (PB_ATYPE(ia.type) != PB_ATYPE_STATIC) {
            continue;
        }
        const bool in_a = mt_present(&ia);
        const bool in_b = mt_present(&ib);
        if (!in_a && !in_b) {
            continue;
        }
        const uint16_t field = parent != 0U ? (uint16_t)(parent * 100U + ia.tag) : (uint16_t)ia.tag;
        /* A message inside the message, both there: walked for its own fields, so a changed
           address inside the network settings is the one line rather than "network changed". */
        if (in_a && in_b && PB_LTYPE_IS_SUBMSG(ia.type) && PB_HTYPE(ia.type) != PB_HTYPE_REPEATED &&
            depth < 2U && ia.tag < 100U && field < 650U) {
            mt_walk(diff, key, field, depth + 1U, ia.submsg_desc, ia.pData, ib.pData);
            continue;
        }
        if (in_a && in_b && mt_same(&ia, &ib)) {
            continue;
        }
        const uint8_t kind = !in_a   ? MESH_RADIO_BACKUP_ADDED
                             : !in_b ? MESH_RADIO_BACKUP_REMOVED
                                     : MESH_RADIO_BACKUP_CHANGED;
        struct mesh_radio_backup_change *change =
            mesh_radio_backup_diff_add(diff, kind, key->topic, key->index, field);
        if (change == NULL) {
            continue;
        }
        if (in_a) {
            mt_value(&ia, &change->before);
        }
        if (in_b) {
            mt_value(&ib, &change->after);
        }
    } while (pb_field_iter_next(&ia) && pb_field_iter_next(&ib));
}

/* Two sections with the same key, compared field by field. */
static void mt_compare(struct mesh_radio_backup_diff *diff, const struct mt_key *key,
                       const uint8_t *data_a, size_t len_a, const union mt_message *a,
                       const uint8_t *data_b, size_t len_b, const union mt_message *b) {
    if (len_a == len_b && memcmp(data_a, data_b, len_a) == 0) {
        return; /* the cheap answer, and the usual one */
    }
    const pb_msgdesc_t *fields = mt_fields(key->tag);
    if (fields == NULL) {
        /* Canned messages and the ringtone: text, compared whole. */
        struct mesh_radio_backup_change *change =
            mesh_radio_backup_diff_add(diff, MESH_RADIO_BACKUP_CHANGED, key->topic, 0U, 0U);
        if (change != NULL) {
            mesh_radio_backup_value_text(&change->before, (const char *)data_a, len_a);
            mesh_radio_backup_value_text(&change->after, (const char *)data_b, len_b);
        }
        return;
    }
    if (key->tag == MESH_RADIO_BACKUP_MT_CONFIG || key->tag == MESH_RADIO_BACKUP_MT_MODULE) {
        const pb_msgdesc_t *variant_a = NULL;
        const pb_msgdesc_t *variant_b = NULL;
        const void *va = NULL;
        const void *vb = NULL;
        pb_size_t tag_a = 0;
        pb_size_t tag_b = 0;
        if (mt_variant(fields, a, &variant_a, &va, &tag_a) &&
            mt_variant(fields, b, &variant_b, &vb, &tag_b) && variant_a == variant_b) {
            mt_walk(diff, key, 0U, 0U, variant_a, va, vb);
        }
        return;
    }
    mt_walk(diff, key, 0U, 0U, fields, a, b);
}

/* A section only one side has: one line for the whole of it. */
static void mt_one_sided(struct mesh_radio_backup_diff *diff, const struct mt_key *key,
                         uint8_t kind) {
    struct mesh_radio_backup_change *change =
        mesh_radio_backup_diff_add(diff, kind, key->topic, key->index, 0U);
    if (change != NULL) {
        mesh_radio_backup_value_opaque(kind == MESH_RADIO_BACKUP_ADDED ? &change->after
                                                                       : &change->before);
    }
}

/* Sections past this are not compared; a Meshtastic backup has about forty. */
#define MT_DIFF_SECTIONS 96U

int mesh_radio_backup_meshtastic_diff(const struct mesh_radio_backup *a,
                                      const struct mesh_radio_backup *b,
                                      struct mesh_radio_backup_diff *out) {
    if (a == NULL || b == NULL || out == NULL) {
        return -EINVAL;
    }
    mesh_radio_backup_diff_reset(out, MESH_RADIO_BACKUP_MESHTASTIC);
    if (a->header.protocol != MESH_RADIO_BACKUP_MESHTASTIC ||
        b->header.protocol != MESH_RADIO_BACKUP_MESHTASTIC) {
        return -EPROTO;
    }
    union mt_message *message_a = malloc(2U * sizeof *message_a);
    if (message_a == NULL) {
        return -ENOMEM;
    }
    union mt_message *message_b = message_a + 1;
    /* Both sides' keys first, since finding a section's partner means knowing what each is. */
    struct mt_key keys_b[MT_DIFF_SECTIONS];
    bool matched[MT_DIFF_SECTIONS] = {false};
    const uint8_t *data = NULL;
    const struct mesh_radio_backup_section *section = NULL;
    size_t count_b = 0U;
    for (; count_b < MT_DIFF_SECTIONS &&
           (section = mesh_radio_backup_section_at(b, count_b, &data)) != NULL;
         ++count_b) {
        keys_b[count_b] = mt_key_of(section->tag, data, section->len, message_b);
    }

    for (size_t i = 0;
         i < MT_DIFF_SECTIONS && (section = mesh_radio_backup_section_at(a, i, &data)) != NULL;
         ++i) {
        const struct mt_key key = mt_key_of(section->tag, data, section->len, message_a);
        if (!key.ok) {
            continue;
        }
        size_t j = 0U;
        while (j < count_b && (matched[j] || !keys_b[j].ok || keys_b[j].tag != key.tag ||
                               keys_b[j].index != key.index)) {
            ++j;
        }
        if (j == count_b) {
            mt_one_sided(out, &key, MESH_RADIO_BACKUP_REMOVED);
            continue;
        }
        matched[j] = true;
        const uint8_t *data_b = NULL;
        const struct mesh_radio_backup_section *other = mesh_radio_backup_section_at(b, j, &data_b);
        (void)mt_key_of(other->tag, data_b, other->len, message_b);
        mt_compare(out, &key, data, section->len, message_a, data_b, other->len, message_b);
    }
    free(message_a);
    for (size_t j = 0; j < count_b; ++j) {
        if (!matched[j] && keys_b[j].ok) {
            mt_one_sided(out, &keys_b[j], MESH_RADIO_BACKUP_ADDED);
        }
    }
    return 0;
}

/* ---- restoring ----------------------------------------------------------------------------- */

/* The admin ConfigType a Config variant is written with: the variants are the types plus one. */
_Static_assert(meshtastic_Config_lora_tag - 1 == meshtastic_AdminMessage_ConfigType_LORA_CONFIG &&
                   meshtastic_Config_security_tag - 1 ==
                       meshtastic_AdminMessage_ConfigType_SECURITY_CONFIG &&
                   meshtastic_Config_device_tag - 1 ==
                       meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG,
               "a Config variant's tag is its ConfigType plus one");

/* One backup section as the write that puts it back; false for a section with no write. */
static bool mt_restore_write(const struct mt_key *key, const union mt_message *message,
                             const uint8_t *data, size_t len,
                             const struct mesh_radio_settings *settings,
                             struct mesh_admin_request *write) {
    memset(write, 0, sizeof *write);
    switch (key->tag) {
    case MESH_RADIO_BACKUP_MT_CONFIG:
        if (message->config.which_payload_variant == 0U) {
            return false;
        }
        write->kind = MESH_ADMIN_SET_CONFIG;
        write->type = (uint32_t)(message->config.which_payload_variant - 1U);
        write->payload.config = message->config;
        if (message->config.which_payload_variant == meshtastic_Config_security_tag) {
            /* The radio's own key pair, never the backup's empty one: see the header. A radio
               that has not told us its private key gets no Security write at all - there is
               no key to carry, and the comparison afterwards lists what was left. */
            if (settings->security.private_key.size != 32U) {
                return false;
            }
            write->payload.config.payload_variant.security.private_key =
                settings->security.private_key;
            write->payload.config.payload_variant.security.public_key =
                settings->security.public_key;
        }
        return true;
    case MESH_RADIO_BACKUP_MT_MODULE:
        for (size_t i = 0; i < mesh_radio_module_count(); ++i) {
            const struct mesh_module_binding *binding = mesh_radio_module_at(i);
            if (binding != NULL && binding->variant_tag == message->module.which_payload_variant) {
                write->kind = MESH_ADMIN_SET_MODULE_CONFIG;
                write->type = binding->admin_type;
                write->payload.module_config = message->module;
                return true;
            }
        }
        return false; /* a module this build keeps no binding for */
    case MESH_RADIO_BACKUP_MT_CHANNEL:
        write->kind = MESH_ADMIN_SET_CHANNEL;
        write->type = (uint32_t)message->channel.index;
        write->payload.channel = message->channel;
        return true;
    case MESH_RADIO_BACKUP_MT_OWNER:
        write->kind = MESH_ADMIN_SET_OWNER;
        write->payload.owner = message->owner;
        write->payload.owner.public_key = settings->owner.public_key;
        return true;
    case MESH_RADIO_BACKUP_MT_UI_CONFIG:
        write->kind = MESH_ADMIN_SET_UI_CONFIG;
        write->payload.ui_config = message->ui;
        return true;
    case MESH_RADIO_BACKUP_MT_CANNED:
        write->kind = MESH_ADMIN_SET_CANNED_MESSAGES;
        return mt_text(data, len, write->payload.text, sizeof write->payload.text);
    case MESH_RADIO_BACKUP_MT_RINGTONE:
        write->kind = MESH_ADMIN_SET_RINGTONE;
        return mt_text(data, len, write->payload.ringtone, sizeof write->payload.ringtone);
    case MESH_RADIO_BACKUP_MT_POSITION:
        write->kind = MESH_ADMIN_SET_FIXED_POSITION;
        write->type = (uint32_t)meshtastic_AdminMessage_ConfigType_POSITION_CONFIG;
        write->payload.position = message->position;
        return true;
    default:
        return false;
    }
}

int mesh_radio_backup_meshtastic_plan(const struct mesh_radio_backup *backup,
                                      const struct mesh_radio_settings *settings,
                                      const struct mesh_handshake_status *status,
                                      struct mesh_admin_request *writes, size_t max) {
    if (backup == NULL || settings == NULL || status == NULL || (writes == NULL && max > 0U)) {
        return -EINVAL;
    }
    if (backup->header.protocol != MESH_RADIO_BACKUP_MESHTASTIC) {
        return -EPROTO;
    }
    struct mesh_radio_backup *live = malloc(sizeof *live);
    union mt_message *message = malloc(sizeof *message);
    if (live == NULL || message == NULL) {
        free(live);
        free(message);
        return -ENOMEM;
    }
    int result = mesh_radio_backup_meshtastic_capture(settings, status, live);
    struct mt_key keys[MT_DIFF_SECTIONS];
    size_t count_live = 0U;
    const uint8_t *data = NULL;
    const struct mesh_radio_backup_section *section = NULL;
    for (; result == 0 && count_live < MT_DIFF_SECTIONS &&
           (section = mesh_radio_backup_section_at(live, count_live, &data)) != NULL;
         ++count_live) {
        keys[count_live] = mt_key_of(section->tag, data, section->len, message);
    }
    size_t planned = 0U;
    for (size_t i = 0; result == 0 && i < MT_DIFF_SECTIONS &&
                       (section = mesh_radio_backup_section_at(backup, i, &data)) != NULL;
         ++i) {
        const struct mt_key key = mt_key_of(section->tag, data, section->len, message);
        if (!key.ok) {
            continue;
        }
        bool same = false;
        for (size_t j = 0; j < count_live; ++j) {
            const uint8_t *live_data = NULL;
            if (keys[j].ok && keys[j].tag == key.tag && keys[j].index == key.index) {
                const struct mesh_radio_backup_section *other =
                    mesh_radio_backup_section_at(live, j, &live_data);
                same = other->len == section->len && memcmp(live_data, data, section->len) == 0;
                break;
            }
        }
        if (same) {
            continue;
        }
        struct mesh_admin_request write;
        if (!mt_restore_write(&key, message, data, section->len, settings, &write)) {
            continue;
        }
        if (planned >= max) {
            result = -ENOSPC;
            break;
        }
        writes[planned++] = write;
    }
    free(live);
    free(message);
    return result == 0 ? (int)planned : result;
}
