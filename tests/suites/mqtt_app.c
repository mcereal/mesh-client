/*
 * The app's half of the MQTT client proxy: deciding whether to be connected at all, to what, and
 * with which subscriptions.
 *
 * All of it is derived from the radio's own MQTTConfig, so the cases here are mostly about
 * reproducing decisions the firmware makes - which means the interesting ones are the places
 * where the obvious behaviour and the firmware's behaviour differ. The credential rule below is
 * the sharpest of those: it is all-or-nothing, and a client that substituted only the address
 * would be refused by the public broker while looking, from the outside, exactly like one whose
 * WiFi was down.
 *
 * Most of these drive mesh_app_mqtt_plan() and its neighbours rather than mesh_app_mqtt_tick(),
 * because a tick that decided to connect would fork a resolver and dial a real broker. The last
 * one does drive the tick, against a proxy built with no event loop so that every start is
 * refused before anything is opened - which is the one path through the tick that the pure
 * functions cannot show.
 */

#include "../../src/core/app_internal.h"
#include "framework/mesh_test.h"
#include "support/mqtt_fixture.h"
#include "support/session_fixture.h"

#include "mesh/core/mqtt_proxy.h"
#include "mesh/core/session.h"
#include "mesh/ui/store_mqtt.h"

#include "meshtastic/config.pb.h"
#include "meshtastic/module_config.pb.h"

#include <stdio.h>
#include <string.h>

/* A radio that has finished its config sync and wants a client to hold its broker connection. */
static meshtastic_ModuleConfig_MQTTConfig proxying_config(void) {
    meshtastic_ModuleConfig_MQTTConfig mqtt = meshtastic_ModuleConfig_MQTTConfig_init_default;
    mqtt.enabled = true;
    mqtt.proxy_to_client_enabled = true;
    return mqtt;
}

/*
 * The session as it stands the moment before a proxy would start: a node number, an MQTT module
 * config, and the flag that says the replay is over.
 *
 * `config_complete` is set rather than earned. Earning it needs a real send path and a matching
 * request id, which would be a fake link in every case here to test a field whose meaning - the
 * sync has finished - is what is under test rather than how it gets set.
 */
static void session_synced(struct mesh_session *session,
                           const meshtastic_ModuleConfig_MQTTConfig *mqtt, uint32_t node_num) {
    mesh_session_init(session);
    session->handshake.has_my_info = true;
    session->handshake.my_info.my_node_num = node_num;
    (void)mesh_test_feed_mqtt(session, mqtt);
    session->handshake.config_complete = true;
}

/* ------------------------------------------------------------------ whether to connect */

MESH_TEST_CASE(mqtt_app_waits_for_the_whole_config_sync, unit) {
    struct mesh_session session;
    const meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    session_synced(&session, &mqtt, 0xABCD1234U);

    struct mesh_mqtt_proxy_config plan;
    if (!mesh_app_mqtt_plan(&session, &plan)) {
        record_failure(test_name, "a synced radio asking to be proxied for should be proxied for");
        return;
    }

    /*
     * Mid-sync, the same radio is not connected to. The module config and the channel table
     * arrive as separate fragments, so a proxy started on the first one would connect with an
     * address about to change and subscribe to a subset of the channels about to arrive.
     */
    session.handshake.config_complete = false;
    if (mesh_app_mqtt_plan(&session, &plan)) {
        record_failure(test_name, "nothing should connect before the sync finishes");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_app_stands_the_connection_down_when_the_radio_goes, unit) {
    struct mesh_session session;
    const meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    session_synced(&session, &mqtt, 0x11223344U);

    struct mesh_mqtt_proxy_config plan;
    if (!mesh_app_mqtt_plan(&session, &plan)) {
        record_failure(test_name, "the radio should be proxied for while it is here");
        return;
    }

    /*
     * The link ending, through the real path rather than by clearing a flag. A proxy is held on
     * behalf of a radio: with none attached there is nothing to publish and nothing that could
     * take a delivery, so the only thing an open socket would still be doing is holding this
     * client's id at the broker.
     *
     * What the radio *said* survives - mesh_session_reset_link_state() deliberately keeps the
     * settings so the next sync has something to draw with - so this case is also the one that
     * would fail if the gate moved to `settings.has_mqtt`, which does not move at all.
     */
    mesh_session_detach(&session);
    if (mesh_app_mqtt_plan(&session, &plan)) {
        record_failure(test_name, "a radio that is gone should not be proxied for");
        return;
    }
    if (!session.settings.has_mqtt) {
        record_failure(test_name, "the radio's own config should outlive the link");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_app_leaves_a_radio_that_reaches_its_own_broker_alone, unit) {
    struct mesh_session session;
    meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    /* The module on, proxying off: a radio that means to reach the broker over its own WiFi.
       Connecting on its behalf would put this mesh on that broker twice. */
    mqtt.proxy_to_client_enabled = false;
    session_synced(&session, &mqtt, 0x11223344U);

    struct mesh_mqtt_proxy_config plan;
    if (mesh_app_mqtt_plan(&session, &plan)) {
        record_failure(test_name, "a radio with its own connection should be left alone");
        return;
    }

    /* And the other half: proxying on with the module off is a radio that is not gatewaying at
       all, whatever the proxy flag still holds. */
    struct mesh_session second;
    meshtastic_ModuleConfig_MQTTConfig off = proxying_config();
    off.enabled = false;
    session_synced(&second, &off, 0x11223344U);
    if (mesh_app_mqtt_plan(&second, &plan)) {
        record_failure(test_name, "a radio with MQTT off should not be proxied for");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ what to connect to */

MESH_TEST_CASE(mqtt_app_takes_the_public_brokers_credentials_with_its_address, unit) {
    struct mesh_session session;
    meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    /* An address left empty, which is the firmware's "the public broker" - but a username filled
       in, which a user who once pointed this at their own broker and then cleared the address
       really does leave behind. */
    snprintf(mqtt.username, sizeof mqtt.username, "%s", "someone");
    snprintf(mqtt.password, sizeof mqtt.password, "%s", "their-secret");
    session_synced(&session, &mqtt, 0x0A0B0C0DU);

    struct mesh_mqtt_proxy_config plan;
    if (!mesh_app_mqtt_plan(&session, &plan)) {
        record_failure(test_name, "the radio asked to be proxied for");
        return;
    }
    /*
     * All three substituted together, which is PubSubConfig's own shape: the firmware reads the
     * username and password only inside `if (*config.address)`. Carrying "someone" to the public
     * broker would be refused - and refused in a way that looks from the Brick exactly like a
     * broker that is down.
     */
    if (strcmp(plan.address, "mqtt.meshtastic.org") != 0) {
        record_failure(test_name, "an empty address means the public broker");
        return;
    }
    if (strcmp(plan.username, "meshdev") != 0 || strcmp(plan.password, "large4cats") != 0) {
        record_failure(test_name, "the public broker comes with its own credentials");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_app_honours_an_empty_username_on_someone_elses_broker, unit) {
    struct mesh_session session;
    meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    snprintf(mqtt.address, sizeof mqtt.address, "%s", "broker.example:1884");
    mqtt.tls_enabled = true;
    session_synced(&session, &mqtt, 0x0A0B0C0DU);

    struct mesh_mqtt_proxy_config plan;
    if (!mesh_app_mqtt_plan(&session, &plan)) {
        record_failure(test_name, "the radio asked to be proxied for");
        return;
    }
    if (strcmp(plan.address, "broker.example:1884") != 0) {
        record_failure(test_name, "the address the radio holds should be used verbatim");
        return;
    }
    /*
     * The other side of the rule above, and the reason it cannot be "substitute when empty": a
     * private broker taking anonymous connections is an ordinary configuration, and sending it
     * "meshdev" would be this client inventing a login the user never typed.
     */
    if (plan.username[0] != '\0' || plan.password[0] != '\0') {
        record_failure(test_name, "an address of their own means credentials of their own");
        return;
    }
    if (!plan.tls_enabled) {
        record_failure(test_name, "tls_enabled is the radio's answer and should be carried");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_app_names_itself_apart_from_the_radio_it_proxies_for, unit) {
    struct mesh_session session;
    const meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    session_synced(&session, &mqtt, 0xABCD1234U);

    struct mesh_mqtt_proxy_config plan;
    if (!mesh_app_mqtt_plan(&session, &plan)) {
        record_failure(test_name, "the radio asked to be proxied for");
        return;
    }
    /*
     * The node id is in there, because two Bricks proxying for two radios must not collide. The
     * prefix is in there because this client and that radio must not collide either: the
     * firmware presents the bare "!abcd1234" when it dials a broker itself, and the moment
     * somebody turns proxying off both would be live under one id - a broker evicts the older
     * holder rather than reporting anything, so the two would take turns kicking each other off
     * for as long as it took the user to notice.
     */
    if (strcmp(plan.client_id, "meshclient-!abcd1234") != 0) {
        record_failure(test_name, "the client id should carry the node id and our own prefix");
        return;
    }
    if (strlen(plan.client_id) >= sizeof plan.client_id) {
        record_failure(test_name, "the client id should fit what a CONNECT will carry");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ noticing a change */

MESH_TEST_CASE(mqtt_app_reconnects_only_when_something_it_dialled_moved, unit) {
    struct mesh_mqtt_proxy_config have;
    memset(&have, 0, sizeof have);
    snprintf(have.address, sizeof have.address, "%s", "broker.example");
    snprintf(have.username, sizeof have.username, "%s", "meshdev");
    snprintf(have.password, sizeof have.password, "%s", "large4cats");
    snprintf(have.client_id, sizeof have.client_id, "%s", "meshclient-!abcd1234");

    struct mesh_mqtt_proxy_config want = have;
    if (mesh_app_mqtt_config_differs(&have, &want)) {
        record_failure(test_name, "an unchanged config should not drop a working connection");
        return;
    }

    /*
     * Each of the five separately, because a comparison that missed one would be a connection
     * that never notices a setting the user just changed - and the failure is silent: it carries
     * on talking to the old broker with the old password and the screen says "connected".
     */
    want = have;
    snprintf(want.address, sizeof want.address, "%s", "broker.example:8883");
    if (!mesh_app_mqtt_config_differs(&have, &want)) {
        record_failure(test_name, "a moved broker should be noticed");
        return;
    }
    want = have;
    snprintf(want.username, sizeof want.username, "%s", "someone");
    if (!mesh_app_mqtt_config_differs(&have, &want)) {
        record_failure(test_name, "a changed username should be noticed");
        return;
    }
    want = have;
    want.password[0] = '\0';
    if (!mesh_app_mqtt_config_differs(&have, &want)) {
        record_failure(test_name, "a cleared password should be noticed");
        return;
    }
    want = have;
    snprintf(want.client_id, sizeof want.client_id, "%s", "meshclient-!00000001");
    if (!mesh_app_mqtt_config_differs(&have, &want)) {
        record_failure(test_name, "a different radio should be noticed");
        return;
    }
    want = have;
    want.tls_enabled = true;
    if (!mesh_app_mqtt_config_differs(&have, &want)) {
        record_failure(test_name, "turning TLS on should be noticed");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ what to subscribe to */

MESH_TEST_CASE(mqtt_app_collects_every_filter_the_session_derives, unit) {
    struct mesh_session session;
    meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    snprintf(mqtt.root, sizeof mqtt.root, "%s", "msh/US");
    session_synced(&session, &mqtt, 0xABCD1234U);
    (void)mesh_test_feed_lora(&session, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, true);
    (void)mesh_test_feed_channel(&session, 0U, "", true);
    (void)mesh_test_feed_channel(&session, 1U, "weather", true);
    (void)mesh_test_feed_channel(&session, 2U, "private", false);

    char filters[MESH_MQTT_FILTERS_MAX][MESH_MQTT_FILTER_MAX];
    const size_t count = mesh_app_mqtt_filters(&session, filters, MESH_MQTT_FILTERS_MAX);
    /* Two downlink channels and the one PKI topic they earn between them. */
    if (count != 3U) {
        record_failure(test_name, "two downlink channels should yield three filters");
        return;
    }
    if (strcmp(filters[0], "msh/US/2/e/LongFast/+") != 0 ||
        strcmp(filters[1], "msh/US/2/e/weather/+") != 0 ||
        strcmp(filters[2], "msh/US/2/e/PKI/+") != 0) {
        record_failure(test_name, "the filters should be the session's, in slot order");
        return;
    }

    /* A radio that wants no downlink at all subscribes to nothing, including PKI. That is an
       answer rather than a failure: it is a radio that means to publish and not to receive. */
    struct mesh_session publisher;
    session_synced(&publisher, &mqtt, 0xABCD1234U);
    (void)mesh_test_feed_lora(&publisher, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, true);
    (void)mesh_test_feed_channel(&publisher, 0U, "", false);
    if (mesh_app_mqtt_filters(&publisher, filters, MESH_MQTT_FILTERS_MAX) != 0U) {
        record_failure(test_name, "a radio that wants no downlink should subscribe to nothing");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_app_stops_at_the_table_it_was_given, unit) {
    struct mesh_session session;
    meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    session_synced(&session, &mqtt, 0xABCD1234U);
    (void)mesh_test_feed_lora(&session, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, true);
    (void)mesh_test_feed_channel(&session, 0U, "one", true);
    (void)mesh_test_feed_channel(&session, 1U, "two", true);
    (void)mesh_test_feed_channel(&session, 2U, "three", true);

    /*
     * Four filters are available and room is given for two. Stopping is the whole behaviour -
     * the proxy's own table would refuse the rest with -ENOSPC anyway, and a count that ran past
     * the caller's array is the one outcome that is not a missed subscription but a corrupted
     * stack.
     */
    char filters[2][MESH_MQTT_FILTER_MAX];
    const size_t count = mesh_app_mqtt_filters(&session, filters, 2U);
    if (count != 2U) {
        record_failure(test_name, "the cap should be the cap");
        return;
    }
    if (strcmp(filters[0], "msh/2/e/one/+") != 0 || strcmp(filters[1], "msh/2/e/two/+") != 0) {
        record_failure(test_name, "the first two filters should be the first two filters");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ acting on it once */

/*
 * The tick, driven against a proxy with no event loop.
 *
 * mesh_mqtt_proxy_init(proxy, NULL) is documented as leaving the proxy permanently OFF, which is
 * what makes this safe to run in a test: mesh_mqtt_proxy_start() refuses with -ENOTSUP before it
 * resolves a name or opens anything. That refusal is also the case worth covering, because the
 * real one it stands in for - an address the radio holds that is not a host - fails exactly the
 * same way and would otherwise be retried, and logged, on every turn of the event loop.
 *
 * `app` is static because struct mesh_app is far too large for a stack frame, and only the four
 * fields mesh_app_mqtt_tick() touches are set up: it reads the session, the proxy, the recorded
 * plan and the kill switch, and nothing else.
 */
MESH_TEST_CASE(mqtt_app_does_not_redial_a_broker_it_was_already_refused, unit) {
    static struct mesh_app app;
    memset(&app, 0, sizeof app);

    meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    session_synced(&app.session, &mqtt, 0xABCD1234U);
    (void)mesh_test_feed_lora(&app.session, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST,
                              true);
    (void)mesh_test_feed_channel(&app.session, 0U, "", true);
    (void)mesh_mqtt_proxy_init(&app.mqtt, NULL);

    mesh_app_mqtt_tick(&app, 1000U);
    if (mesh_mqtt_proxy_state(&app.mqtt) != MESH_MQTT_PROXY_OFF) {
        record_failure(test_name, "a proxy with no loop should not have started");
        return;
    }
    /* Recorded despite the refusal, which is the whole point: this is what the next turn is
       compared against. */
    if (!app.mqtt_planned.active ||
        strcmp(app.mqtt_planned.config.client_id, "meshclient-!abcd1234") != 0) {
        record_failure(test_name, "a refused attempt should still be recorded");
        return;
    }
    if (app.mqtt_planned.filter_count != 2U) {
        record_failure(test_name, "the subscriptions it would have made should be recorded too");
        return;
    }

    /* Nothing about the radio moved, so nothing is tried again. */
    char filters[MESH_MQTT_FILTERS_MAX][MESH_MQTT_FILTER_MAX];
    struct mesh_mqtt_proxy_config want;
    if (!mesh_app_mqtt_plan(&app.session, &want)) {
        record_failure(test_name, "the radio still wants a proxy");
        return;
    }
    const size_t count = mesh_app_mqtt_filters(&app.session, filters, MESH_MQTT_FILTERS_MAX);
    if (mesh_app_mqtt_plan_changed(&app.mqtt_planned, &want, filters, count)) {
        record_failure(test_name, "an unchanged radio should not be dialled again");
        return;
    }

    /*
     * A changed one is. The address is what the user would have gone and fixed, and correcting
     * it has to be the thing that makes this try again - nothing else ever will.
     */
    snprintf(mqtt.address, sizeof mqtt.address, "%s", "broker.example");
    (void)mesh_test_feed_mqtt(&app.session, &mqtt);
    if (!mesh_app_mqtt_plan(&app.session, &want)) {
        record_failure(test_name, "the radio still wants a proxy");
        return;
    }
    if (!mesh_app_mqtt_plan_changed(&app.mqtt_planned, &want, filters, count)) {
        record_failure(test_name, "a corrected address should be dialled");
        return;
    }

    /* And so is a subscription set that shrank, which no amount of adding filters would fix. */
    if (!mesh_app_mqtt_plan_changed(&app.mqtt_planned, &app.mqtt_planned.config, filters, 1U)) {
        record_failure(test_name, "a channel that stopped downlinking should be noticed");
        return;
    }

    /* The radio going stands it down, and clears the record with it: the next radio to arrive
       must not be measured against the last one's plan. */
    mesh_session_detach(&app.session);
    mesh_app_mqtt_tick(&app, 2000U);
    if (app.mqtt_planned.active) {
        record_failure(test_name, "a link that dropped should clear what was planned");
        return;
    }
    mesh_mqtt_proxy_shutdown(&app.mqtt);
    record_success(test_name);
}

/* ------------------------------------------------------------------ what the screen reads */

/*
 * The four numbers mesh/ui/store_mqtt.h restates on the far side of the seam.
 *
 * That header is nanopb-free and names no core module by construction, so it cannot say
 * `MESH_MQTT_ADDRESS_MAX` and has to carry its own copy - the same arrangement
 * MESH_UI_NETWORK_HOST_MAX and MESH_WAYPOINT_NAME_MAX already have. What holds a restated
 * constant honest is a case like this one: the failure it prevents is a broker name that is
 * fine everywhere in the client and clipped on the one screen that exists to name it.
 */
MESH_TEST_CASE(mqtt_status_limits_agree_across_the_seam, unit) {
    if (MESH_UI_MQTT_HOST_MAX < MESH_MQTT_ADDRESS_MAX) {
        record_failure(test_name, "the published host should hold any address the radio can");
        return;
    }
    struct mesh_mqtt_proxy probe;
    (void)mesh_mqtt_proxy_init(&probe, NULL);
    if (MESH_UI_MQTT_ERROR_MAX < sizeof probe.last_error) {
        record_failure(test_name, "the published reason should hold any reason the proxy writes");
        return;
    }
    /* Every sentence the core's own table can produce, including the default arm: a state added
       to the enum without a string still comes back as something, and that something has to fit
       too. */
    for (int state = 0; state <= (int)MESH_MQTT_PROXY_STATE_COUNT; ++state) {
        const char *text = mesh_mqtt_proxy_state_string((enum mesh_mqtt_proxy_state)state);
        if (text == NULL || strlen(text) >= MESH_UI_MQTT_STATE_MAX) {
            record_failure(test_name, "a connection state does not fit the published field");
            return;
        }
    }
    mesh_mqtt_proxy_shutdown(&probe);
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_status_says_nothing_about_a_radio_that_never_asked, unit) {
    static struct mesh_app app;
    memset(&app, 0, sizeof app);
    meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    mqtt.proxy_to_client_enabled = false;
    session_synced(&app.session, &mqtt, 0xABCD1234U);
    (void)mesh_mqtt_proxy_init(&app.mqtt, NULL);

    struct mesh_ui_mqtt_state ui;
    /* Deliberately dirty, so a publish that forgot to clear the record would be caught rather
       than passing on whatever the caller's stack held. */
    memset(&ui, 0xA5, sizeof ui);
    mesh_app_mqtt_publish_state(&app, &ui);

    /*
     * The whole card is drawn on `wanted`, so this is the case that decides whether almost every
     * Brick in the world spends four rows of its Status screen saying "Off" about a feature
     * nobody turned on.
     */
    if (ui.wanted) {
        record_failure(test_name, "a radio that is not proxying should draw no card");
        return;
    }
    if (ui.host[0] != '\0' || ui.state[0] != '\0' || ui.subscriptions != 0U) {
        record_failure(test_name, "the record should be cleared, not left as it was found");
        return;
    }
    mesh_mqtt_proxy_shutdown(&app.mqtt);
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_status_names_the_broker_the_radio_did_not, unit) {
    static struct mesh_app app;
    memset(&app, 0, sizeof app);
    const meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    session_synced(&app.session, &mqtt, 0xABCD1234U);
    (void)mesh_test_feed_lora(&app.session, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST,
                              true);
    (void)mesh_test_feed_channel(&app.session, 0U, "", true);
    (void)mesh_mqtt_proxy_init(&app.mqtt, NULL);
    mesh_app_mqtt_tick(&app, 1000U);

    struct mesh_ui_mqtt_state ui;
    mesh_app_mqtt_publish_state(&app, &ui);
    if (!ui.wanted) {
        record_failure(test_name, "a radio asking to be proxied for should draw the card");
        return;
    }
    /*
     * The row the radio's own Settings screen cannot draw. `MQTTConfig.address` is empty here -
     * which is the ordinary configuration - so the Server field over in Settings is blank, and
     * the substitution that turns it into a real broker happens on this client.
     */
    if (strcmp(ui.host, "mqtt.meshtastic.org") != 0) {
        record_failure(test_name, "the card should name the broker the plan resolved");
        return;
    }
    /*
     * Two subscriptions from one downlink channel, read off the recorded plan rather than off
     * the proxy - which in this test has no event loop and therefore holds none. That is the
     * case the choice was made for: a start the proxy refused would otherwise report "none
     * subscribed" and blame the radio's channels for the client's own refusal.
     */
    if (ui.subscriptions != 2U) {
        record_failure(test_name, "the card should count the subscriptions that were planned");
        return;
    }
    if (ui.connected || ui.failing) {
        record_failure(test_name, "a proxy that never started is neither connected nor failing");
        return;
    }
    mesh_mqtt_proxy_shutdown(&app.mqtt);
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_status_tells_the_clients_refusal_from_the_radios_silence, unit) {
    static struct mesh_app app;
    memset(&app, 0, sizeof app);
    const meshtastic_ModuleConfig_MQTTConfig mqtt = proxying_config();
    session_synced(&app.session, &mqtt, 0xABCD1234U);
    (void)mesh_mqtt_proxy_init(&app.mqtt, NULL);
    app.mqtt_disabled = true;
    /* The radio offering messages nobody is taking, which is what this looks like from its side
       and is the only number that moves while the client is declining. */
    app.session.mqtt_unhandled = 57U;

    struct mesh_ui_mqtt_state ui;
    mesh_app_mqtt_publish_state(&app, &ui);
    /*
     * Both true at once, and that is the point of keeping them apart. The radio *is* asking, so
     * the card is drawn; this client is refusing, so what it says is about the Brick. Folding
     * the two into one flag would have left the card either absent - hiding the refusal from the
     * one person who could undo it - or indistinguishable from a broker that is down.
     */
    if (!ui.wanted || !ui.disabled) {
        record_failure(test_name, "a declined radio is still a radio that asked");
        return;
    }
    if (ui.unhandled != 57U) {
        record_failure(test_name, "what the radio offered and nobody took should be carried");
        return;
    }
    mesh_mqtt_proxy_shutdown(&app.mqtt);
    record_success(test_name);
}
