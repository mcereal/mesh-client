#include "support/backup_fixture.h"

#include <stdio.h>
#include <string.h>

void mesh_test_backup_radio(struct mesh_radio_settings *settings,
                            struct mesh_handshake_status *status) {
    mesh_radio_settings_reset(settings);
    memset(status, 0, sizeof *status);
    status->has_my_info = true;
    status->my_info.my_node_num = 0x0badcafeU;
    status->my_info.nodedb_count = 57U;

    static const pb_size_t tags[] = {
        meshtastic_Config_device_tag,    meshtastic_Config_position_tag,
        meshtastic_Config_power_tag,     meshtastic_Config_network_tag,
        meshtastic_Config_display_tag,   meshtastic_Config_lora_tag,
        meshtastic_Config_bluetooth_tag, meshtastic_Config_security_tag,
    };
    for (size_t i = 0; i < sizeof tags / sizeof tags[0]; ++i) {
        meshtastic_Config config = meshtastic_Config_init_zero;
        config.which_payload_variant = tags[i];
        if (tags[i] == meshtastic_Config_lora_tag) {
            config.payload_variant.lora.use_preset = true;
            config.payload_variant.lora.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
            config.payload_variant.lora.modem_preset =
                meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST;
            config.payload_variant.lora.tx_power = 17;
            config.payload_variant.lora.hop_limit = 5U;
        } else if (tags[i] == meshtastic_Config_security_tag) {
            config.payload_variant.security.private_key.size = 32U;
            memset(config.payload_variant.security.private_key.bytes, 0xAB, 32U);
            config.payload_variant.security.public_key.size = 32U;
            memset(config.payload_variant.security.public_key.bytes, 0x5C, 32U);
        } else if (tags[i] == meshtastic_Config_device_tag) {
            config.payload_variant.device.role = meshtastic_Config_DeviceConfig_Role_ROUTER;
        }
        mesh_radio_settings_apply_config(settings, &config);
    }

    meshtastic_User owner = meshtastic_User_init_zero;
    snprintf(owner.long_name, sizeof owner.long_name, "Ridge relay");
    snprintf(owner.short_name, sizeof owner.short_name, "RDG");
    mesh_radio_settings_apply_owner(settings, &owner);

    meshtastic_DeviceMetadata metadata = meshtastic_DeviceMetadata_init_zero;
    snprintf(metadata.firmware_version, sizeof metadata.firmware_version, "2.7.15.567b8ea");
    metadata.hw_model = meshtastic_HardwareModel_HELTEC_V3;
    mesh_radio_settings_apply_metadata(settings, &metadata);

    for (int8_t slot = 0; slot < (int8_t)MESH_MESHTASTIC_CHANNELS; ++slot) {
        meshtastic_Channel channel = meshtastic_Channel_init_zero;
        channel.index = slot;
        /* A slot switched off comes with empty settings, as a Heltec V3 on 2.7.26 sends it. */
        channel.has_settings = true;
        if (slot == 0) {
            channel.role = meshtastic_Channel_Role_PRIMARY;
            channel.has_settings = true;
            channel.settings.psk.size = 1U;
            channel.settings.psk.bytes[0] = 1U;
        } else if (slot == 2) {
            channel.role = meshtastic_Channel_Role_SECONDARY;
            channel.has_settings = true;
            snprintf(channel.settings.name, sizeof channel.settings.name, "Ops");
            channel.settings.psk.size = 16U;
            memset(channel.settings.psk.bytes, 0x42, 16U);
        }
        mesh_radio_settings_apply_channel(settings, &channel);
    }

    meshtastic_ModuleConfig module = meshtastic_ModuleConfig_init_zero;
    module.which_payload_variant = meshtastic_ModuleConfig_telemetry_tag;
    module.payload_variant.telemetry.device_update_interval = 1800U;
    mesh_radio_settings_apply_module_config(settings, &module);

    settings->has_canned_messages = true;
    snprintf(settings->canned_messages, sizeof settings->canned_messages, "OK|On my way|Help");
}
