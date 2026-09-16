#ifndef MESH_TEST_SUPPORT_MQTT_FIXTURE_H
#define MESH_TEST_SUPPORT_MQTT_FIXTURE_H

/*
 * The config-sync fragments everything MQTT is derived from, delivered the way a radio delivers
 * them: three separate FromRadio messages through the real decode path.
 *
 * Fed rather than assigned because the derivation reads across sections that arrive apart - the
 * module config, the LoRa config and the channel table - and a case that set the session's
 * fields directly would be testing its own idea of where they land rather than the session's.
 */

#include "mesh/core/session.h"

#include "meshtastic/config.pb.h"
#include "meshtastic/module_config.pb.h"

#include <stdbool.h>
#include <stdint.h>

bool mesh_test_feed_lora(struct mesh_session *session,
                         meshtastic_Config_LoRaConfig_ModemPreset preset, bool use_preset);

bool mesh_test_feed_mqtt(struct mesh_session *session,
                         const meshtastic_ModuleConfig_MQTTConfig *mqtt);

/* Slot 0 is the primary and everything above it a secondary, which is where every radio puts
   them. `name` may be empty, which is the unnamed default channel. */
bool mesh_test_feed_channel(struct mesh_session *session, uint8_t index, const char *name,
                            bool downlink);

#endif /* MESH_TEST_SUPPORT_MQTT_FIXTURE_H */
