#include "mesh/proto/mqtt_topic.h"

#include <errno.h>
#include <string.h>

/* The fixed middle of every topic: protocol version 2, `e` for the encrypted envelope. */
#define MQTT_TOPIC_INFIX "/2/e/"
/* The one level a subscription leaves open, which is the gateway node id. */
#define MQTT_TOPIC_NODE_WILDCARD "/+"
/* Direct messages are addressed to a node rather than carried on a channel, so they arrive on a
   channel id of their own rather than on any of the configured ones. */
#define MQTT_TOPIC_PKI "PKI"

const char *mesh_mqtt_preset_name(meshtastic_Config_LoRaConfig_ModemPreset preset,
                                  bool use_preset) {
    /*
     * The firmware's early return, and the reason it is first here too: a radio with hand-set
     * bandwidth is on "Custom" whatever `modem_preset` still holds. Reading the preset first
     * and the flag second would name a preset the radio is not using.
     */
    if (!use_preset) {
        return "Custom";
    }

    switch (preset) {
    case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO:
        return "ShortTurbo";
    case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW:
        return "ShortSlow";
    case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST:
        return "ShortFast";
    case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW:
        return "MediumSlow";
    case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST:
        return "MediumFast";
    case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_TURBO:
        return "MediumTurbo";
    case meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW:
        return "LongSlow";
    case meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST:
        return "LongFast";
    case meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO:
        return "LongTurbo";
    /* "LongMod", not "LongModerate". Upstream's abbreviation, and it is on the wire. */
    case meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE:
        return "LongMod";
    case meshtastic_Config_LoRaConfig_ModemPreset_LITE_FAST:
        return "LiteFast";
    case meshtastic_Config_LoRaConfig_ModemPreset_LITE_SLOW:
        return "LiteSlow";
    case meshtastic_Config_LoRaConfig_ModemPreset_NARROW_FAST:
        return "NarrowFast";
    case meshtastic_Config_LoRaConfig_ModemPreset_NARROW_SLOW:
        return "NarrowSlow";
    case meshtastic_Config_LoRaConfig_ModemPreset_TINY_FAST:
        return "TinyFast";
    case meshtastic_Config_LoRaConfig_ModemPreset_TINY_SLOW:
        return "TinySlow";
    /*
     * VERY_LONG_SLOW is deliberately absent, and this is not an oversight being carried
     * forward twice.
     *
     * The firmware's switch has no case for it either - it was deprecated in 2.5 as "works only
     * with txco and is unusably slow" - so a radio still on it takes the default branch and
     * calls its unnamed channel **"Invalid"**. That string is what such a radio puts in the
     * topic it publishes to, so it is the string to subscribe to. Naming it "VeryLongSlow"
     * here would be correct English and a silent one-way mesh.
     */
    default:
        return "Invalid";
    }
}

const char *mesh_mqtt_channel_id(const char *name, const meshtastic_Config_LoRaConfig *lora) {
    if (name != NULL && name[0] != '\0') {
        return name;
    }
    /*
     * No LoRa config yet means no preset to name, and the handshake has not finished. "Custom"
     * is what the firmware says for a radio whose preset does not apply, which is the closest
     * true thing to say about a radio that has not told us its preset - and a caller deriving
     * filters this early is going to re-derive them when the config lands anyway.
     */
    if (lora == NULL) {
        return "Custom";
    }
    return mesh_mqtt_preset_name(lora->modem_preset, lora->use_preset);
}

/* True for a topic component a broker would read as something other than a literal name. A `/`
   is included because these are single components: one arriving with a slash in it would
   silently add a level and change what the filter matches. */
static bool mqtt_component_is_plain(const char *text) {
    for (const char *at = text; *at != '\0'; ++at) {
        if (*at == '+' || *at == '#' || *at == '/') {
            return false;
        }
    }
    return true;
}

/* The shared body of both filters: everything but which channel id goes in the middle. */
static int mqtt_build_filter(char *out, size_t cap, const char *root, const char *channel_id) {
    if (out == NULL || cap == 0U || channel_id == NULL || channel_id[0] == '\0') {
        return -EINVAL;
    }
    if (root == NULL || root[0] == '\0') {
        root = MESH_MQTT_ROOT_DEFAULT;
    }
    /*
     * A wildcard in either half would widen the subscription past this mesh - `#` in a root
     * subscribes to every mesh on a shared broker - and both come from configuration a phone
     * wrote, so neither is trusted here. The root is allowed its slashes, since "msh/US" is the
     * ordinary way to write one; the channel id is not, because it is one component.
     */
    if (!mqtt_component_is_plain(channel_id)) {
        return -EINVAL;
    }
    for (const char *at = root; *at != '\0'; ++at) {
        if (*at == '+' || *at == '#') {
            return -EINVAL;
        }
    }

    const size_t root_len = strlen(root);
    const size_t infix_len = strlen(MQTT_TOPIC_INFIX);
    const size_t id_len = strlen(channel_id);
    const size_t tail_len = strlen(MQTT_TOPIC_NODE_WILDCARD);
    const size_t needed = root_len + infix_len + id_len + tail_len;
    /*
     * Measured in full before a byte is written, so a refusal leaves `out` untouched rather
     * than holding a prefix of the right filter. A truncated filter is the dangerous failure
     * here: it would still be a legal topic and would still subscribe, just to the wrong thing.
     */
    if (needed + 1U > cap) {
        return -ENOSPC;
    }

    size_t at = 0U;
    memcpy(out + at, root, root_len);
    at += root_len;
    memcpy(out + at, MQTT_TOPIC_INFIX, infix_len);
    at += infix_len;
    memcpy(out + at, channel_id, id_len);
    at += id_len;
    memcpy(out + at, MQTT_TOPIC_NODE_WILDCARD, tail_len);
    at += tail_len;
    out[at] = '\0';
    return (int)at;
}

int mesh_mqtt_subscribe_filter(char *out, size_t cap, const char *root, const char *channel_id) {
    return mqtt_build_filter(out, cap, root, channel_id);
}

int mesh_mqtt_pki_filter(char *out, size_t cap, const char *root) {
    return mqtt_build_filter(out, cap, root, MQTT_TOPIC_PKI);
}
