#include "mesh/core/radio_backup_meshtastic.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <errno.h>
#include <stdio.h>
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
