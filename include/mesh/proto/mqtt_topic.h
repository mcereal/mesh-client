#ifndef MESH_PROTO_MQTT_TOPIC_H
#define MESH_PROTO_MQTT_TOPIC_H

/*
 * Where a Meshtastic mesh lives on an MQTT broker.
 *
 * `<root>/2/e/<channel>/<!nodeid>` - a root the radio's owner chose, a protocol version, `e` for
 * the encrypted envelope, the channel's global id, and the gateway node that published it.
 *
 * This exists because of an asymmetry in the client proxy protocol. Publishing needs no topic
 * derivation at all: the radio builds the topic itself and hands it over in
 * `MqttClientProxyMessage.topic`, and the proxy's job is to put those bytes on that topic.
 * *Subscribing* has no such message. `MQTT::sendSubscriptions()` in the firmware is wrapped
 * entirely in `#if HAS_NETWORKING`, so a radio proxying through a client never subscribes to
 * anything and never says what it would have subscribed to. A client that wants downlink has to
 * work the topics out for itself, from the same configuration the radio used.
 *
 * Which makes this a compatibility surface rather than a design: every string here has to match
 * what the firmware would have produced, character for character, or the subscription is to a
 * topic nobody publishes on and the mesh is silently one-way. It is `src/proto/` for the reason
 * channel_url.c is - a wire format and nothing else, with no radio and no socket in it.
 */

#include "meshtastic/config.pb.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the firmware uses when `MQTTConfig.root` is empty. */
#define MESH_MQTT_ROOT_DEFAULT "msh"

/*
 * The longest filter these can build: a 31-character root, "/2/e/", an 11-character channel id,
 * and "/+". `INKWELL_MQTT_CLIENT_FILTER_MAX` in inkwell/net/mqtt.h is comfortably above it.
 */
#define MESH_MQTT_FILTER_NEEDED 50U

/*
 * The firmware's name for a modem preset, which is also the default name of a channel that has
 * none - `DisplayFormatters::getModemPresetDisplayName(preset, false, use_preset)`.
 *
 * Reproduced rather than improved, and the two places it is wrong are the two that matter:
 *
 *   - `LONG_MODERATE` is "LongMod", not "LongModerate".
 *   - `VERY_LONG_SLOW` has no case in the firmware's switch at all, so it falls through to the
 *     default and comes out as **"Invalid"**. A radio on that preset with an unnamed channel
 *     really does publish to `.../2/e/Invalid/...`.
 *
 * Both are what goes on the wire, so both are what has to be matched. `use_preset` false is the
 * firmware's own early return: a radio with hand-set bandwidth is on "Custom" whatever its
 * preset field happens to say.
 *
 * Never NULL, and always a literal that outlives the call.
 */
const char *mesh_mqtt_preset_name(meshtastic_Config_LoRaConfig_ModemPreset preset, bool use_preset);

/*
 * A channel's global id: its name, or the preset name when it has none.
 *
 * `Channels::getGlobalId()` is `getName()` with a `// FIXME, not correct` beside it upstream,
 * and `getName()` substitutes the preset name for an empty one. `name` may be NULL or empty.
 * The result is either `name` itself or a literal, so it lives as long as whichever it was.
 */
const char *mesh_mqtt_channel_id(const char *name, const meshtastic_Config_LoRaConfig *lora);

/*
 * Writes `<root>/2/e/<channel_id>/+` and returns its length, or -EINVAL for an empty channel id
 * or a root or id carrying a wildcard or a `/`, or -ENOSPC when `cap` is too small.
 *
 * A NULL or empty `root` becomes "msh", which is the firmware's default. Nothing else about the
 * root is touched - **a trailing slash is not stripped**. The firmware concatenates
 * `moduleConfig.mqtt.root + "/2/e/"` with no normalisation, so a radio configured with "msh/"
 * genuinely publishes to "msh//2/e/...", and a client that tidied that up would subscribe to a
 * topic its own radio is not using.
 *
 * `+` rather than `#`, matching the firmware: one level, which is the gateway node id.
 */
int mesh_mqtt_subscribe_filter(char *out, size_t cap, const char *root, const char *channel_id);

/*
 * Writes `<root>/2/e/PKI/+`, the topic direct-message traffic arrives on.
 *
 * The firmware subscribes to this once, if *any* channel has downlink enabled, rather than once
 * per channel - PKI messages are addressed to a node rather than carried on a channel. Same
 * returns as above.
 */
int mesh_mqtt_pki_filter(char *out, size_t cap, const char *root);

#ifdef __cplusplus
}
#endif

#endif /* MESH_PROTO_MQTT_TOPIC_H */
