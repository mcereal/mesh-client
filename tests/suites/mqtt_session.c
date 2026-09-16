/*
 * The session's half of the MQTT client proxy: the FromRadio variant coming out, the ToRadio
 * going back, and the subscriptions neither of them carries.
 *
 * The asymmetry is the thing worth holding onto while reading this. Publishing is a relay - the
 * radio builds the topic and this client is only a route to the internet - so those cases check
 * that nothing is interpreted. Subscribing has no message at all: the firmware's
 * MQTT::sendSubscriptions() is compiled out entirely when there is no networking, so a proxying
 * radio never says what it would have subscribed to and the client has to derive it from the
 * same configuration. Those cases drive the real config sync rather than setting struct fields,
 * because the derivation reads three sections that arrive as three separate fragments.
 */

#include "framework/mesh_test.h"
#include "support/mqtt_fixture.h"
#include "support/session_fixture.h"

#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/channel.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/module_config.pb.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* What the handler was handed, so a case can check it rather than the session's own state. */
struct proxy_capture {
    unsigned calls;
    char topic[80];
    uint8_t payload[64];
    size_t len;
    bool retained;
};

static void proxy_capture_fn(void *ctx, const char *topic, const uint8_t *payload, size_t len,
                             bool retained) {
    struct proxy_capture *capture = (struct proxy_capture *)ctx;
    capture->calls++;
    snprintf(capture->topic, sizeof capture->topic, "%s", topic);
    capture->len = len < sizeof capture->payload ? len : sizeof capture->payload;
    if (capture->len > 0U && payload != NULL) {
        memcpy(capture->payload, payload, capture->len);
    }
    capture->retained = retained;
}

/* A FromRadio carrying one proxy message, fed through the real decode path. */
static bool feed_proxy_message(struct mesh_session *session, const char *topic, const uint8_t *data,
                               size_t len, bool retained) {
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_mqttClientProxyMessage_tag;
    meshtastic_MqttClientProxyMessage *message = &from_radio.mqttClientProxyMessage;
    snprintf(message->topic, sizeof message->topic, "%s", topic);
    message->which_payload_variant = meshtastic_MqttClientProxyMessage_data_tag;
    message->payload_variant.data.size = (pb_size_t)len;
    memcpy(message->payload_variant.data.bytes, data, len);
    message->retained = retained;
    return mesh_test_session_feed_from_radio(session, &from_radio);
}

/* ------------------------------------------------------------------ the radio publishing */

MESH_TEST_CASE(mqtt_session_relays_what_the_radio_asked_for, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    struct proxy_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_set_mqtt_handler(&session, proxy_capture_fn, &capture);

    static const uint8_t envelope[] = {0x0AU, 0x03U, 0xDEU, 0xADU, 0xBEU};
    if (!feed_proxy_message(&session, "msh/US/2/e/LongFast/!abcd1234", envelope, sizeof envelope,
                            true)) {
        record_failure(test_name, "the session should decode a proxy message");
        return;
    }

    if (capture.calls != 1U) {
        record_failure(test_name, "the handler should have been called once");
        return;
    }
    /* The topic is the radio's and arrives untouched: a proxy is a route, not an opinion. */
    if (strcmp(capture.topic, "msh/US/2/e/LongFast/!abcd1234") != 0) {
        record_failure(test_name, "the radio's topic should be passed through unchanged");
        return;
    }
    if (capture.len != sizeof envelope || memcmp(capture.payload, envelope, sizeof envelope) != 0) {
        record_failure(test_name, "the payload should arrive whole");
        return;
    }
    if (!capture.retained) {
        record_failure(test_name, "the retained flag is the radio's and should be carried");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_session_reads_the_text_variant_as_its_own_length, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    struct proxy_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_set_mqtt_handler(&session, proxy_capture_fn, &capture);

    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_mqttClientProxyMessage_tag;
    meshtastic_MqttClientProxyMessage *message = &from_radio.mqttClientProxyMessage;
    snprintf(message->topic, sizeof message->topic, "msh/2/e/LongFast/!1");
    message->which_payload_variant = meshtastic_MqttClientProxyMessage_text_tag;
    snprintf(message->payload_variant.text, sizeof message->payload_variant.text, "hello");

    if (!mesh_test_session_feed_from_radio(&session, &from_radio)) {
        record_failure(test_name, "a text proxy message should decode");
        return;
    }
    /*
     * Five bytes, measured with strnlen rather than read out of `data.size`. The two share
     * storage - they are a union - so a text message whose length was read from the wrong half
     * would announce whatever its own first characters happen to spell.
     */
    if (capture.calls != 1U || capture.len != 5U || memcmp(capture.payload, "hello", 5U) != 0) {
        record_failure(test_name, "text should be measured as a string, not as a byte count");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_session_counts_a_message_with_nowhere_to_go, unit) {
    struct mesh_session session;
    mesh_session_init(&session);
    /* No handler: a radio with proxying on and a client that is not proxying, which is an
       ordinary configuration rather than a fault. */

    static const uint8_t envelope[] = {0x01U, 0x02U};
    for (unsigned i = 0U; i < 3U; ++i) {
        if (!feed_proxy_message(&session, "msh/2/e/LongFast/!1", envelope, sizeof envelope,
                                false)) {
            record_failure(test_name, "the message should still decode without a handler");
            return;
        }
    }
    if (session.mqtt_unhandled != 3U) {
        record_failure(test_name, "messages with no proxy running should be counted");
        return;
    }

    /* And the count is about this link, so it goes when the link does. */
    mesh_session_detach(&session);
    if (session.mqtt_unhandled != 0U) {
        record_failure(test_name, "the count should not outlive the connection");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_session_ignores_a_message_carrying_no_payload, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    struct proxy_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_set_mqtt_handler(&session, proxy_capture_fn, &capture);

    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_mqttClientProxyMessage_tag;
    snprintf(from_radio.mqttClientProxyMessage.topic,
             sizeof from_radio.mqttClientProxyMessage.topic, "msh/2/e/LongFast/!1");
    /* No payload_variant set at all, which the oneof allows and the firmware warns about. */

    if (!mesh_test_session_feed_from_radio(&session, &from_radio)) {
        record_failure(test_name, "the message should decode");
        return;
    }
    if (capture.calls != 0U) {
        record_failure(test_name, "a message with no payload should not reach the handler");
        return;
    }

    /* And the same for a topic that is not there to publish on. */
    memset(&from_radio, 0, sizeof from_radio);
    from_radio.which_payload_variant = meshtastic_FromRadio_mqttClientProxyMessage_tag;
    from_radio.mqttClientProxyMessage.which_payload_variant =
        meshtastic_MqttClientProxyMessage_data_tag;
    from_radio.mqttClientProxyMessage.payload_variant.data.size = 1U;
    if (!mesh_test_session_feed_from_radio(&session, &from_radio) || capture.calls != 0U) {
        record_failure(test_name, "a message with no topic should not reach the handler");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ the broker answering */

/* Pulls the MqttClientProxyMessage back out of whatever the session handed the link. */
static bool decode_sent_proxy(const struct mesh_test_trace_capture *capture,
                              meshtastic_MqttClientProxyMessage *out) {
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t stream = pb_istream_from_buffer(capture->packet, capture->len);
    if (!pb_decode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
        return false;
    }
    if (to_radio.which_payload_variant != meshtastic_ToRadio_mqttClientProxyMessage_tag) {
        return false;
    }
    *out = to_radio.mqttClientProxyMessage;
    return true;
}

MESH_TEST_CASE(mqtt_session_hands_the_broker_reply_to_the_radio, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    struct mesh_test_trace_capture sent;
    memset(&sent, 0, sizeof sent);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &sent);
    /* The firmware drops this variant until the config sync is done, so a case that means to
       test the encoding has to get past that gate first. */
    session.handshake.config_complete = true;

    static const uint8_t envelope[] = {0x12U, 0x04U, 0x00U, 0xFFU, 0x00U, 0x7FU};
    const int result =
        mesh_session_send_mqtt_proxy(&session, "msh/2/e/PKI/!deadbeef", envelope, sizeof envelope);
    if (result != 0 || sent.calls != 1U) {
        record_failure(test_name, "a broker message should reach the radio");
        return;
    }

    meshtastic_MqttClientProxyMessage message = meshtastic_MqttClientProxyMessage_init_default;
    if (!decode_sent_proxy(&sent, &message)) {
        record_failure(test_name, "the ToRadio should carry a proxy message");
        return;
    }
    if (strcmp(message.topic, "msh/2/e/PKI/!deadbeef") != 0) {
        record_failure(test_name, "the topic should be the one the broker used");
        return;
    }
    /*
     * The `data` variant, always. `text` is accepted by the firmware and measured with
     * strnlen(), which for an encrypted envelope - a payload largely made of zero bytes - would
     * hand the radio a fraction of the message and no indication that it had.
     */
    if (message.which_payload_variant != meshtastic_MqttClientProxyMessage_data_tag) {
        record_failure(test_name, "a broker payload should go over as bytes, never as text");
        return;
    }
    if (message.payload_variant.data.size != sizeof envelope ||
        memcmp(message.payload_variant.data.bytes, envelope, sizeof envelope) != 0) {
        record_failure(test_name, "the payload should arrive whole, zero bytes included");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_session_will_not_talk_during_the_config_sync, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    struct mesh_test_trace_capture sent;
    memset(&sent, 0, sizeof sent);

    static const uint8_t envelope[] = {0x01U};

    /* No link at all. */
    if (mesh_session_send_mqtt_proxy(&session, "msh/2/e/PKI/!1", envelope, sizeof envelope) !=
        -ENOTCONN) {
        record_failure(test_name, "with no link this should be -ENOTCONN");
        return;
    }

    /*
     * A link, but mid-handshake. PhoneAPI refuses the variant outright while it is still
     * syncing - "Ignore MqttClientProxy msg during config handshake" - so sending now spends a
     * round trip to have the radio discard it in silence.
     */
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &sent);
    if (mesh_session_send_mqtt_proxy(&session, "msh/2/e/PKI/!1", envelope, sizeof envelope) !=
        -ENOTCONN) {
        record_failure(test_name, "before config_complete this should be -ENOTCONN");
        return;
    }
    if (sent.calls != 0U) {
        record_failure(test_name, "nothing should have gone to the radio");
        return;
    }

    session.handshake.config_complete = true;
    if (mesh_session_send_mqtt_proxy(&session, "msh/2/e/PKI/!1", envelope, sizeof envelope) != 0) {
        record_failure(test_name, "once the sync is done it should go");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_session_refuses_more_than_the_radios_field_holds, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    struct mesh_test_trace_capture sent;
    memset(&sent, 0, sizeof sent);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &sent);
    session.handshake.config_complete = true;

    /*
     * MqttClientProxyMessage.data is 435 bytes and a broker may send more. An encrypted
     * envelope cannot be truncated - half of one decodes to nothing - so it is refused whole
     * rather than cut down, and the caller counts the drop.
     */
    static uint8_t oversized[436];
    memset(oversized, 0xA5U, sizeof oversized);
    if (mesh_session_send_mqtt_proxy(&session, "msh/2/e/PKI/!1", oversized, sizeof oversized) !=
        -EMSGSIZE) {
        record_failure(test_name, "a payload past the radio's field should be refused");
        return;
    }
    if (sent.calls != 0U) {
        record_failure(test_name, "a refused message should not be half-sent");
        return;
    }
    if (mesh_session_send_mqtt_proxy(&session, "msh/2/e/PKI/!1", oversized, 435U) != 0) {
        record_failure(test_name, "exactly the field's size should still go");
        return;
    }

    /* A topic longer than the radio's own 60-byte field goes the same way. */
    static const char long_topic[] =
        "msh/a-very-long-root-that-somebody-typed/2/e/LongFast/!deadbeef";
    if (mesh_session_send_mqtt_proxy(&session, long_topic, oversized, 4U) != -EMSGSIZE) {
        record_failure(test_name, "a topic past the radio's field should be refused");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_session_refuses_a_length_with_no_bytes_behind_it, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    struct mesh_test_trace_capture sent;
    memset(&sent, 0, sizeof sent);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &sent);
    session.handshake.config_complete = true;

    /*
     * The failure this guards against is not a crash. Skipping the copy and sending the zeroed
     * field anyway would hand the radio a full-length envelope of zeros that decodes to nothing,
     * report success, and tell nobody - a caller's bad argument becoming silent corruption on
     * the mesh.
     */
    if (mesh_session_send_mqtt_proxy(&session, "msh/2/e/PKI/!1", NULL, 16U) != -EINVAL) {
        record_failure(test_name, "a NULL payload with a length should be refused");
        return;
    }
    if (sent.calls != 0U) {
        record_failure(test_name, "nothing should have gone to the radio");
        return;
    }

    /* A NULL with no length is an empty message, which MQTT has and which is not this bug. */
    if (mesh_session_send_mqtt_proxy(&session, "msh/2/e/PKI/!1", NULL, 0U) != 0) {
        record_failure(test_name, "an empty message should still be allowed");
        return;
    }
    meshtastic_MqttClientProxyMessage message = meshtastic_MqttClientProxyMessage_init_default;
    if (!decode_sent_proxy(&sent, &message) || message.payload_variant.data.size != 0U) {
        record_failure(test_name, "an empty message should carry no bytes");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ what to subscribe to */

/*
 * The fragments the derivation reads are fed from support/mqtt_fixture.h, since the app's half
 * of the proxy is derived from the same sync. Only the root matters to the cases below, so this
 * is the shorthand for "an MQTT config that says nothing else".
 */
static bool feed_mqtt_root(struct mesh_session *session, const char *root) {
    meshtastic_ModuleConfig_MQTTConfig mqtt = meshtastic_ModuleConfig_MQTTConfig_init_default;
    snprintf(mqtt.root, sizeof mqtt.root, "%s", root);
    return mesh_test_feed_mqtt(session, &mqtt);
}

/* Collects the whole derived set, so a case can compare it as a list. */
static size_t collect_filters(const struct mesh_session *session, char out[][64], size_t max) {
    size_t count = 0U;
    while (count < max) {
        const int written = mesh_session_mqtt_filter(session, count, out[count], 64U);
        if (written <= 0) {
            break;
        }
        count++;
    }
    return count;
}

MESH_TEST_CASE(mqtt_session_subscribes_where_the_firmware_would_have, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    if (!feed_mqtt_root(&session, "msh/US") ||
        !mesh_test_feed_lora(&session, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, true) ||
        !mesh_test_feed_channel(&session, 0U, "", true) ||        /* the unnamed default primary */
        !mesh_test_feed_channel(&session, 1U, "weather", true) || /* a named secondary */
        !mesh_test_feed_channel(&session, 2U, "private", false)) {
        record_failure(test_name, "the config sync should be accepted");
        return;
    }

    char filters[8][64];
    const size_t count = collect_filters(&session, filters, 8U);

    /*
     * Three: one per downlink-enabled channel in slot order, then PKI. The channel that does
     * not downlink is absent, and the unnamed one is subscribed to by its preset name - which is
     * what the radio itself publishes under.
     */
    if (count != 3U) {
        record_failure(test_name, "one filter per downlink channel, plus PKI");
        return;
    }
    if (strcmp(filters[0], "msh/US/2/e/LongFast/+") != 0) {
        record_failure(test_name, "an unnamed channel subscribes under its preset name");
        return;
    }
    if (strcmp(filters[1], "msh/US/2/e/weather/+") != 0) {
        record_failure(test_name, "a named channel subscribes under its name");
        return;
    }
    if (strcmp(filters[2], "msh/US/2/e/PKI/+") != 0) {
        record_failure(test_name, "direct messages arrive on PKI, once, last");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_session_subscribes_to_nothing_without_downlink, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    if (!feed_mqtt_root(&session, "msh") ||
        !mesh_test_feed_lora(&session, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, true) ||
        !mesh_test_feed_channel(&session, 0U, "", false) ||
        !mesh_test_feed_channel(&session, 1U, "weather", false)) {
        record_failure(test_name, "the config sync should be accepted");
        return;
    }

    char filters[8][64];
    /*
     * Not even PKI. A radio that downlinks on no channel is one that means to publish and not
     * to receive, and handing it other people's direct messages would undo the setting rather
     * than honour it - which is why the firmware gates its own PKI subscribe on the same flag.
     */
    if (collect_filters(&session, filters, 8U) != 0U) {
        record_failure(test_name, "a radio with no downlink should subscribe to nothing");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_session_derives_filters_before_the_sync_finishes, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    /* A channel has arrived; the LoRa fragment and the MQTT module have not. The set has to be
       answerable anyway, because the fragments arrive in whatever order the radio sends them. */
    if (!mesh_test_feed_channel(&session, 0U, "", true)) {
        record_failure(test_name, "a channel should be accepted on its own");
        return;
    }

    char filters[8][64];
    const size_t count = collect_filters(&session, filters, 8U);
    if (count != 2U) {
        record_failure(test_name, "a partial sync should still yield a set");
        return;
    }
    /* "msh" because no root has arrived, "Custom" because no preset has. Both are what the
       firmware says in the same situation, and the set is re-derived when the rest lands. */
    if (strcmp(filters[0], "msh/2/e/Custom/+") != 0 || strcmp(filters[1], "msh/2/e/PKI/+") != 0) {
        record_failure(test_name, "the defaults should be the firmware's");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_session_reports_a_buffer_that_cannot_hold_a_filter, unit) {
    struct mesh_session session;
    mesh_session_init(&session);
    if (!feed_mqtt_root(&session, "msh/US") ||
        !mesh_test_feed_lora(&session, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, true) ||
        !mesh_test_feed_channel(&session, 0U, "", true)) {
        record_failure(test_name, "the config sync should be accepted");
        return;
    }

    char small[8];
    if (mesh_session_mqtt_filter(&session, 0U, small, sizeof small) != -ENOSPC) {
        record_failure(test_name, "a buffer too small should say so rather than truncate");
        return;
    }
    /* And past the end is 0 - not an error, and how a caller knows it has them all. */
    char filter[64];
    if (mesh_session_mqtt_filter(&session, 9U, filter, sizeof filter) != 0) {
        record_failure(test_name, "an index past the end should be 0");
        return;
    }
    record_success(test_name);
}

/*
 * A channel edited from this client, which arrives by a different door than the sync's.
 *
 * Found on hardware and not by anything above, because everything above feeds channels the way
 * the config sync does - as a FromRadio - and that path writes both of the two tables a channel
 * lives in. An edit made from the Settings screen comes back as an AdminMessage
 * get_channel_response instead, which lands in mesh_radio_settings_apply_channel() and writes
 * only `settings`. The derivation was reading `handshake.channels[]`, so turning downlink on
 * from the Brick left it subscribed to nothing while the row that had just been pressed said
 * "on" - the Settings screen reads the table the admin reply updates.
 *
 * The two paths are checked against each other rather than separately: one channel in by each
 * door, and the filters have to follow both.
 */
MESH_TEST_CASE(mqtt_session_follows_a_channel_edited_from_here, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    if (!feed_mqtt_root(&session, "msh/US/PR") ||
        !mesh_test_feed_lora(&session, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, true) ||
        !mesh_test_feed_channel(&session, 0U, "", false)) {
        record_failure(test_name, "the config sync should be accepted");
        return;
    }

    char filters[MESH_SESSION_MAX_CHANNELS + 1U][64];
    if (collect_filters(&session, filters, MESH_SESSION_MAX_CHANNELS + 1U) != 0U) {
        record_failure(test_name, "a channel with downlink off should subscribe to nothing");
        return;
    }

    /*
     * The same channel back with downlink on, by the door an admin reply uses. Nothing here
     * touches the handshake's own copy, which is the whole point: this is what the radio really
     * sends after a set_channel, and the derivation has to be reading the table it lands in.
     */
    meshtastic_Channel edited = meshtastic_Channel_init_default;
    edited.index = 0;
    edited.role = meshtastic_Channel_Role_PRIMARY;
    edited.has_settings = true;
    edited.settings.uplink_enabled = true;
    edited.settings.downlink_enabled = true;
    mesh_radio_settings_apply_channel(&session.settings, &edited);

    const size_t count = collect_filters(&session, filters, MESH_SESSION_MAX_CHANNELS + 1U);
    if (count != 2U) {
        record_failure(test_name, "an edited channel should be subscribed to");
        return;
    }
    /* The unnamed primary takes the preset's name, and PKI comes with it because something now
       downlinks - both of which the edit has to carry, not just the flag. */
    if (strcmp(filters[0], "msh/US/PR/2/e/LongFast/+") != 0 ||
        strcmp(filters[1], "msh/US/PR/2/e/PKI/+") != 0) {
        record_failure(test_name, "the edited channel should name the same topics as a synced one");
        return;
    }
    record_success(test_name);
}
