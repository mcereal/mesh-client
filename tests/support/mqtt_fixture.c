#include "support/mqtt_fixture.h"

#include "support/session_fixture.h"

#include "meshtastic/channel.pb.h"
#include "meshtastic/mesh.pb.h"

#include <stdio.h>

bool mesh_test_feed_lora(struct mesh_session *session,
                         meshtastic_Config_LoRaConfig_ModemPreset preset, bool use_preset) {
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_config_tag;
    from_radio.config.which_payload_variant = meshtastic_Config_lora_tag;
    from_radio.config.payload_variant.lora.use_preset = use_preset;
    from_radio.config.payload_variant.lora.modem_preset = preset;
    return mesh_test_session_feed_from_radio(session, &from_radio);
}

bool mesh_test_feed_mqtt(struct mesh_session *session,
                         const meshtastic_ModuleConfig_MQTTConfig *mqtt) {
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_moduleConfig_tag;
    from_radio.moduleConfig.which_payload_variant = meshtastic_ModuleConfig_mqtt_tag;
    from_radio.moduleConfig.payload_variant.mqtt = *mqtt;
    return mesh_test_session_feed_from_radio(session, &from_radio);
}

bool mesh_test_feed_channel(struct mesh_session *session, uint8_t index, const char *name,
                            bool downlink) {
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_channel_tag;
    from_radio.channel.index = (int8_t)index;
    from_radio.channel.role =
        index == 0U ? meshtastic_Channel_Role_PRIMARY : meshtastic_Channel_Role_SECONDARY;
    from_radio.channel.has_settings = true;
    snprintf(from_radio.channel.settings.name, sizeof from_radio.channel.settings.name, "%s", name);
    from_radio.channel.settings.downlink_enabled = downlink;
    from_radio.channel.settings.uplink_enabled = true;
    return mesh_test_session_feed_from_radio(session, &from_radio);
}
