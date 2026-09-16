/*
 * Where a mesh lives on a broker, checked against the firmware rather than against taste.
 *
 * Every expected string here was read out of the Meshtastic firmware - `MQTT::sendSubscriptions`
 * for the shape, `Channels::getName` for the channel id, and
 * `DisplayFormatters::getModemPresetDisplayName` for the preset table. That is the whole
 * standard these have to meet: a filter that is *sensible* but not what the radio's own client
 * would have produced subscribes to a topic nobody publishes on, and the failure is a mesh that
 * publishes fine and receives nothing. There is no error to observe, which is why the strings
 * are pinned here instead.
 */

#include "framework/mesh_test.h"

#include "mesh/proto/mqtt_topic.h"

#include <errno.h>
#include <string.h>

static bool preset_named(meshtastic_Config_LoRaConfig_ModemPreset preset, const char *want) {
    return strcmp(mesh_mqtt_preset_name(preset, true), want) == 0;
}

/* ------------------------------------------------------------------ the preset table */

MESH_TEST_CASE(mqtt_topic_names_every_preset_the_firmware_does, unit) {
    static const struct {
        meshtastic_Config_LoRaConfig_ModemPreset preset;
        const char *name;
    } expected[] = {
        {meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, "LongFast"},
        {meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW, "LongSlow"},
        {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW, "MediumSlow"},
        {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST, "MediumFast"},
        {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW, "ShortSlow"},
        {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST, "ShortFast"},
        {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO, "ShortTurbo"},
        {meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO, "LongTurbo"},
        {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_TURBO, "MediumTurbo"},
        {meshtastic_Config_LoRaConfig_ModemPreset_LITE_FAST, "LiteFast"},
        {meshtastic_Config_LoRaConfig_ModemPreset_LITE_SLOW, "LiteSlow"},
        {meshtastic_Config_LoRaConfig_ModemPreset_NARROW_FAST, "NarrowFast"},
        {meshtastic_Config_LoRaConfig_ModemPreset_NARROW_SLOW, "NarrowSlow"},
        {meshtastic_Config_LoRaConfig_ModemPreset_TINY_FAST, "TinyFast"},
        {meshtastic_Config_LoRaConfig_ModemPreset_TINY_SLOW, "TinySlow"},
    };

    for (size_t i = 0U; i < sizeof expected / sizeof expected[0]; ++i) {
        if (!preset_named(expected[i].preset, expected[i].name)) {
            record_failure(test_name, "a preset is not named the way the firmware names it");
            return;
        }
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_topic_keeps_the_firmwares_two_odd_preset_names, unit) {
    /*
     * The two entries somebody will eventually try to "fix". Both are on the wire.
     *
     * LONG_MODERATE is abbreviated to "LongMod" upstream while every other long name is spelled
     * out; VERY_LONG_SLOW has no case in the firmware's switch at all - it was deprecated in 2.5
     * - so it falls to the default and a radio still using it calls its unnamed channel
     * "Invalid".
     */
    if (!preset_named(meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE, "LongMod")) {
        record_failure(test_name, "LONG_MODERATE is 'LongMod' on the wire, not 'LongModerate'");
        return;
    }
    if (!preset_named(meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW, "Invalid")) {
        record_failure(test_name, "VERY_LONG_SLOW has no firmware case and comes out 'Invalid'");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_topic_calls_a_hand_tuned_radio_custom, unit) {
    /* use_preset false is the firmware's early return, before the preset is even looked at: a
       radio with hand-set bandwidth is on "Custom" whatever modem_preset still holds. */
    const char *name =
        mesh_mqtt_preset_name(meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, false);
    if (strcmp(name, "Custom") != 0) {
        record_failure(test_name, "a radio not using a preset is on 'Custom'");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ the channel id */

MESH_TEST_CASE(mqtt_topic_prefers_the_channel_name_it_was_given, unit) {
    meshtastic_Config_LoRaConfig lora = meshtastic_Config_LoRaConfig_init_default;
    lora.use_preset = true;
    lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST;

    if (strcmp(mesh_mqtt_channel_id("weather", &lora), "weather") != 0) {
        record_failure(test_name, "a named channel keeps its name");
        return;
    }
    /* The default primary channel has no name at all, and the preset is what stands in. */
    if (strcmp(mesh_mqtt_channel_id("", &lora), "MediumFast") != 0) {
        record_failure(test_name, "an unnamed channel takes the preset name");
        return;
    }
    if (strcmp(mesh_mqtt_channel_id(NULL, &lora), "MediumFast") != 0) {
        record_failure(test_name, "a NULL name is an unnamed channel");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_topic_survives_a_radio_that_has_not_said_its_preset, unit) {
    if (strcmp(mesh_mqtt_channel_id(NULL, NULL), "Custom") != 0) {
        record_failure(test_name, "no LoRa config should not crash or invent a preset");
        return;
    }
    /* A name still wins: it needs no config to be true. */
    if (strcmp(mesh_mqtt_channel_id("admin", NULL), "admin") != 0) {
        record_failure(test_name, "a named channel needs no config");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ the filters */

MESH_TEST_CASE(mqtt_topic_builds_the_filter_the_firmware_subscribes_to, unit) {
    char filter[64];
    const int written = mesh_mqtt_subscribe_filter(filter, sizeof filter, "msh/US", "LongFast");
    if (written < 0 || strcmp(filter, "msh/US/2/e/LongFast/+") != 0) {
        record_failure(test_name, "a filter should be <root>/2/e/<channel>/+");
        return;
    }
    if ((size_t)written != strlen(filter)) {
        record_failure(test_name, "the returned length should be the string's");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_topic_defaults_the_root_to_msh, unit) {
    char from_null[64];
    char from_empty[64];
    if (mesh_mqtt_subscribe_filter(from_null, sizeof from_null, NULL, "LongFast") < 0 ||
        mesh_mqtt_subscribe_filter(from_empty, sizeof from_empty, "", "LongFast") < 0) {
        record_failure(test_name, "an absent root should be accepted");
        return;
    }
    if (strcmp(from_null, "msh/2/e/LongFast/+") != 0 ||
        strcmp(from_empty, "msh/2/e/LongFast/+") != 0) {
        record_failure(test_name, "an absent root is 'msh', as the firmware has it");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_topic_does_not_tidy_a_trailing_slash, unit) {
    /*
     * The firmware concatenates `moduleConfig.mqtt.root + "/2/e/"` with no normalisation, so a
     * radio configured with "msh/" really does publish to "msh//2/e/...". Stripping the slash
     * here would be tidier and would subscribe to a topic this radio is not using.
     */
    char filter[64];
    if (mesh_mqtt_subscribe_filter(filter, sizeof filter, "msh/", "LongFast") < 0 ||
        strcmp(filter, "msh//2/e/LongFast/+") != 0) {
        record_failure(test_name, "a trailing slash in the root is the radio's, and is kept");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_topic_puts_direct_messages_on_pki, unit) {
    char filter[64];
    if (mesh_mqtt_pki_filter(filter, sizeof filter, "msh/EU_868") < 0 ||
        strcmp(filter, "msh/EU_868/2/e/PKI/+") != 0) {
        record_failure(test_name, "the direct-message filter should be <root>/2/e/PKI/+");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_topic_refuses_what_would_widen_the_subscription, unit) {
    char filter[64];
    /*
     * Both halves come from configuration some phone wrote, and a `#` in either is a
     * subscription to every mesh on a shared broker rather than to this one. A `/` in a channel
     * id is the quieter version of the same thing: it adds a level, so the filter stops matching
     * what it was meant to and starts matching something else.
     */
    if (mesh_mqtt_subscribe_filter(filter, sizeof filter, "msh/#", "LongFast") != -EINVAL) {
        record_failure(test_name, "a wildcard root should be refused");
        return;
    }
    if (mesh_mqtt_subscribe_filter(filter, sizeof filter, "msh/+/x", "LongFast") != -EINVAL) {
        record_failure(test_name, "a single-level wildcard root should be refused too");
        return;
    }
    if (mesh_mqtt_subscribe_filter(filter, sizeof filter, "msh", "Long#Fast") != -EINVAL) {
        record_failure(test_name, "a wildcard channel id should be refused");
        return;
    }
    if (mesh_mqtt_subscribe_filter(filter, sizeof filter, "msh", "Long/Fast") != -EINVAL) {
        record_failure(test_name, "a channel id with a slash adds a level and is refused");
        return;
    }
    if (mesh_mqtt_subscribe_filter(filter, sizeof filter, "msh", "") != -EINVAL) {
        record_failure(test_name, "an empty channel id should be refused");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_topic_writes_nothing_into_a_buffer_too_small, unit) {
    char filter[64];
    memset(filter, 'x', sizeof filter);
    filter[sizeof filter - 1U] = '\0';

    /* "msh/2/e/LongFast/+" is 18 characters, so 18 bytes is one short of holding it. */
    if (mesh_mqtt_subscribe_filter(filter, 18U, "msh", "LongFast") != -ENOSPC) {
        record_failure(test_name, "a buffer one byte short should be refused");
        return;
    }
    /*
     * And refused without writing, which matters more here than it usually does: a truncated
     * filter is still a legal topic, so it would subscribe rather than fail, and it would
     * subscribe to the wrong thing.
     */
    for (size_t i = 0U; i < sizeof filter - 1U; ++i) {
        if (filter[i] != 'x') {
            record_failure(test_name, "a refused filter should leave the buffer alone");
            return;
        }
    }
    if (mesh_mqtt_subscribe_filter(filter, 19U, "msh", "LongFast") != 18) {
        record_failure(test_name, "exactly enough room should be enough");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_topic_fits_the_longest_configuration_in_a_filter, unit) {
    /*
     * The worst case the protobufs allow: a 31-character root and an 11-character channel name,
     * which is what MESH_MQTT_FILTER_NEEDED is sized from. If this ever stops fitting, the
     * proxy's own filter table is the thing to grow.
     */
    static const char root[] = "0123456789012345678901234567890"; /* 31, the max */
    static const char channel[] = "01234567890";                  /* 11, the max */
    char filter[MESH_MQTT_FILTER_NEEDED + 1U];

    const int written = mesh_mqtt_subscribe_filter(filter, sizeof filter, root, channel);
    if (written < 0) {
        record_failure(test_name, "the largest legal configuration should still fit");
        return;
    }
    if ((size_t)written > MESH_MQTT_FILTER_NEEDED) {
        record_failure(test_name, "MESH_MQTT_FILTER_NEEDED understates the longest filter");
        return;
    }
    record_success(test_name);
}
