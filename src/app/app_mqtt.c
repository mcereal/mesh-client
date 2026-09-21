#define _POSIX_C_SOURCE 200809L

/*
 * The broker connection, held on behalf of whichever radio is attached.
 *
 * The three modules under this one each know a piece and none of them knows the whole: the proxy
 * (mesh/core/mqtt_proxy.h) owns one socket and refuses to invent an address, the topic builder
 * (mesh/proto/mqtt_topic.h) owns the wire format and has never seen a radio, and the session
 * (mesh/core/session.h) owns the link and hands over messages without an opinion about where
 * they go. What is left is the decision itself - *whether* to be connected, to *what*, and with
 * which subscriptions - and it is here because this is the only place that can see the radio's
 * configuration and the event loop at the same time.
 *
 * The whole of it is derived, every turn, from the radio's own MQTTConfig. There is no client-
 * side broker setting and there should not be one: the radio is the origin, it decides where its
 * mesh is published, and a client with its own idea of the broker would be publishing this mesh
 * somewhere its own radio is not listening. The client's only say is the kill switch below.
 */

#include "inkwell/base/env.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"

#include "app_internal.h"

#include "inkwell/net/reason.h"
#include "mesh/core/mqtt_proxy.h"
#include "mesh/core/session.h"
#include "mesh/core/tls_client.h"
#include "mesh/ui/mqtt.h"
#include "mesh/ui/store_mqtt.h"

#include <stdio.h>
#include <string.h>

/*
 * What the firmware connects to when `MQTTConfig.address` is empty, reproduced exactly.
 *
 * And reproduced *together*, which is the part that is easy to get wrong. The firmware's
 * PubSubConfig substitutes all three or none of them:
 *
 *     if (*config.address) { serverAddr = config.address;
 *                            mqttUsername = config.username;
 *                            mqttPassword = config.password; }
 *
 * so an empty address means the public broker with the well-known credentials, and a *set*
 * address means whatever the user typed even when that is nothing - a private broker that takes
 * anonymous connections is a real configuration and the firmware honours it. Substituting only
 * the address would send "meshdev" to somebody's own broker, or, in the other direction, send a
 * blank username to the public one and be refused. The Android proxy carries the same rule with
 * the same reasoning beside it.
 */
#define APP_MQTT_DEFAULT_ADDRESS "mqtt.meshtastic.org"
#define APP_MQTT_DEFAULT_USERNAME "meshdev"
#define APP_MQTT_DEFAULT_PASSWORD "large4cats"

bool mesh_app_mqtt_plan(const struct mesh_session *session, struct mesh_mqtt_proxy_config *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (session == NULL) {
        return false;
    }

    /*
     * The config sync, not merely the link.
     *
     * Two of the three things needed here arrive late and separately - the MQTT module config
     * and the channel table - so a proxy started on the first fragment would connect with an
     * address that was about to change and subscribe to a subset of the channels that were about
     * to arrive, then tear itself down and do it again. Waiting for the whole picture costs the
     * seventeen seconds of a replay, during which the radio is not publishing anything either.
     *
     * It is also what stands the connection down when the link drops, since this is the one
     * field mesh_session_reset_link_state() clears. That is the intended reading of "on behalf
     * of a radio": with no radio there is nothing to publish and nothing that could take a
     * delivery, so holding the socket open would only keep the client id claimed at the broker.
     */
    if (!session->handshake.config_complete) {
        return false;
    }
    if (!session->settings.has_mqtt) {
        return false;
    }

    const meshtastic_ModuleConfig_MQTTConfig *mqtt = &session->settings.mqtt;
    /*
     * Both flags, because they are two different questions and the firmware asks both. `enabled`
     * is whether this radio gateways to MQTT at all; `proxy_to_client_enabled` is whether it
     * expects somebody else to hold the connection. A radio with the module enabled and proxying
     * off is one that means to reach a broker over its own WiFi, and connecting on its behalf
     * would put its mesh on a broker twice.
     */
    if (!mqtt->enabled || !mqtt->proxy_to_client_enabled) {
        return false;
    }

    /*
     * No node number, no client id. Unreachable in practice - the config sync sends MyNodeInfo
     * long before it says it is complete - but the alternative to checking is every radio that
     * has not said presenting "!00000000" to the same broker and evicting each other.
     */
    if (!session->handshake.has_my_info) {
        return false;
    }

    if (mqtt->address[0] != '\0') {
        (void)inkwell_str_copy(out->address, sizeof out->address, mqtt->address);
        (void)inkwell_str_copy(out->username, sizeof out->username, mqtt->username);
        (void)inkwell_str_copy(out->password, sizeof out->password, mqtt->password);
    } else {
        (void)inkwell_str_copy(out->address, sizeof out->address, APP_MQTT_DEFAULT_ADDRESS);
        (void)inkwell_str_copy(out->username, sizeof out->username, APP_MQTT_DEFAULT_USERNAME);
        (void)inkwell_str_copy(out->password, sizeof out->password, APP_MQTT_DEFAULT_PASSWORD);
    }
    out->tls_enabled = mqtt->tls_enabled;

    /*
     * The radio's node id with our own prefix on it, rather than the bare "!%08x" the firmware
     * presents when it holds the connection itself.
     *
     * The prefix is the point. A client id is unique or it is nothing: a broker does not report
     * a collision, it disconnects the older holder, so two clients sharing one would take turns
     * evicting each other and reconnecting for as long as both were running. Using the radio's
     * own id would collide with the radio itself the moment somebody turns proxying off - the
     * firmware then dials the same broker under that exact id - which is precisely the window in
     * which a user is watching to see whether the change worked.
     *
     * The Android proxy went further and appends a random UUID per connection, because a phone
     * can be proxying for a node whose id it does not know yet and the whole "unknown" pool
     * collides. This has no such pool: the gate above means there is always a node number by the
     * time anything connects.
     */
    (void)snprintf(out->client_id, sizeof out->client_id, "meshclient-!%08x",
                   session->handshake.my_info.my_node_num);
    return true;
}

bool mesh_app_mqtt_config_differs(const struct mesh_mqtt_proxy_config *have,
                                  const struct mesh_mqtt_proxy_config *want) {
    if (have == NULL || want == NULL) {
        return have != want;
    }
    /*
     * Field by field rather than memcmp, which would also be comparing whatever the padding
     * after `tls_enabled` happens to hold. Both structs here are memset before they are filled,
     * so today it would agree - and it would stop agreeing the first time one of them is built
     * some other way, silently, as a connection that never reconnects after a config change.
     */
    return strcmp(have->address, want->address) != 0 ||
           strcmp(have->username, want->username) != 0 ||
           strcmp(have->password, want->password) != 0 ||
           strcmp(have->client_id, want->client_id) != 0 || have->tls_enabled != want->tls_enabled;
}

size_t mesh_app_mqtt_filters(const struct mesh_session *session, char (*out)[MESH_MQTT_FILTER_MAX],
                             size_t cap) {
    if (session == NULL || out == NULL) {
        return 0U;
    }
    size_t count = 0U;
    while (count < cap) {
        const int len = mesh_session_mqtt_filter(session, count, out[count], MESH_MQTT_FILTER_MAX);
        if (len == 0) {
            break;
        }
        if (len < 0) {
            /*
             * A filter the session could derive but this buffer cannot hold, or a channel name
             * carrying a wildcard. Skipping it and carrying on would subscribe to *some* of what
             * the radio wanted, which reads on screen as a working proxy that silently never
             * delivers on one channel. Stopping here is not better in itself, but it is
             * consistent with what the caller then does: the same short set arrives on every
             * turn, so nothing flaps, and the count is what a status row can compare against the
             * channels the user can see.
             */
            inkwell_log_warn("mqtt", "Cannot build subscription %zu: %d", count, len);
            break;
        }
        count++;
    }
    return count;
}

/*
 * Whether what the proxy was last told to do is already exactly this.
 *
 * `filters` is not const, which it should be and cannot be: C will not implicitly convert
 * `char (*)[N]` to `const char (*)[N]`, because qualifying an array type qualifies its elements
 * and that makes the two pointee types incompatible rather than merely differently qualified.
 * Every caller passes a local array it is done writing to.
 */
bool mesh_app_mqtt_plan_changed(const struct mesh_app_mqtt_plan *planned,
                                const struct mesh_mqtt_proxy_config *want,
                                char (*filters)[MESH_MQTT_FILTER_MAX], size_t count) {
    if (planned == NULL || want == NULL) {
        return true;
    }
    if (!planned->active) {
        return true;
    }
    if (mesh_app_mqtt_config_differs(&planned->config, want)) {
        return true;
    }
    if (planned->filter_count != count) {
        return true;
    }
    for (size_t i = 0U; i < count; ++i) {
        if (filters == NULL || strcmp(planned->filters[i], filters[i]) != 0) {
            return true;
        }
    }
    return false;
}

/* One message the radio wants put on the broker. */
static void app_mqtt_from_radio(void *ctx, const char *topic, const uint8_t *payload, size_t len,
                                bool retained) {
    struct mesh_app *app = (struct mesh_app *)ctx;
    if (app == NULL) {
        return;
    }
    /*
     * The result is deliberately dropped. Every way this can fail is already counted in the
     * proxy's `dropped`, and the ordinary failure - the broker being unreachable - happens once
     * per position report for as long as the link is down. A log line per message would bury the
     * one line that says why.
     */
    (void)mesh_mqtt_proxy_publish(&app->mqtt, topic, payload, len, retained);
}

/* One message from the broker, on a topic this radio's configuration asked for. */
static void app_mqtt_from_broker(void *userdata, const char *topic, const uint8_t *payload,
                                 size_t len) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL) {
        return;
    }
    if (mesh_session_send_mqtt_proxy(&app->session, topic, payload, len) < 0) {
        /*
         * Counted here because neither side counts it: the proxy has already booked this one as
         * `received`, and the session has no counter for a send it refused. Both ways it can
         * happen are real - the link dropping between this turn's tick and this callback, and a
         * broker message that fits the proxy's buffer but not the radio's 435-byte field.
         */
        app->mqtt_undelivered++;
    }
}

static void app_mqtt_state_changed(void *userdata, enum mesh_mqtt_proxy_state state) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL) {
        return;
    }
    /*
     * Only the failure. The proxy logs its own CONNACK - "Connected to %s as %s" - and a second
     * line here saying the same thing in different words is two entries per connection in a log
     * somebody is reading to find out why there is none.
     *
     * The reason a failure needs saying *here* is that the proxy records it and carries on: it
     * goes into `last_error` for the Status card and nothing prints it, so on a device whose
     * screen is not being watched the retry loop would be silent about what it was retrying.
     */
    if (state == MESH_MQTT_PROXY_WAITING) {
        /*
         * The reason's name, not the sentence a screen would show. This line used to print the
         * translated text, so a Spanish device wrote its retry loop in Spanish - and a log is
         * read by whoever is debugging it, not by whoever is holding the Brick. The proxy logs
         * the same reason with its detail and its backoff; this is the app's own note that the
         * loop is running at all.
         */
        const struct mesh_mqtt_proxy_failure failure = mesh_mqtt_proxy_failure(&app->mqtt);
        inkwell_log_warn("mqtt", "Broker %s: %s", mesh_mqtt_proxy_host(&app->mqtt),
                         mesh_mqtt_failure_name(&failure));
    }
}

static void app_mqtt_stop(struct mesh_app *app) {
    /*
     * The handler goes with the connection rather than staying installed for the life of the
     * app, so that mesh_session's `mqtt_unhandled` keeps meaning what it says: proxy messages a
     * radio offered with nowhere to put them. Left installed while the proxy is off, every one
     * of them would instead be booked as a publish this client dropped, which is a different
     * sentence - "the broker would not take it" rather than "nobody is proxying".
     */
    mesh_session_set_mqtt_handler(&app->session, NULL, NULL);
    mesh_mqtt_proxy_stop(&app->mqtt);
    mesh_mqtt_proxy_clear_filters(&app->mqtt);
    memset(&app->mqtt_planned, 0, sizeof app->mqtt_planned);
}

static void app_mqtt_start(struct mesh_app *app, const struct mesh_mqtt_proxy_config *want,
                           char (*filters)[MESH_MQTT_FILTER_MAX], size_t count, uint64_t now_ms) {
    /* Through stop() rather than straight into start(), which closes the socket without saying
       anything: a reconfiguration is deliberate, and a broker told nothing holds the session
       open until the keepalive expires and fires any will message it was given. */
    app_mqtt_stop(app);

    /*
     * Recorded before the outcome is known, and recorded even when the outcome is a refusal.
     *
     * This is what the next turn compares against, and a refusal is deterministic in the
     * configuration that caused it - an address the radio holds that is not a host stays not a
     * host. Recording only successes would re-attempt it, and re-log it, every turn of the event
     * loop for as long as the radio held that address. When the user corrects it the plan
     * differs again and this runs again, which is the only retry that could ever help.
     */
    app->mqtt_planned.config = *want;
    app->mqtt_planned.filter_count = count;
    for (size_t i = 0U; i < count; ++i) {
        (void)inkwell_str_copy(app->mqtt_planned.filters[i], MESH_MQTT_FILTER_MAX, filters[i]);
    }
    app->mqtt_planned.active = true;

    const int started = mesh_mqtt_proxy_start(&app->mqtt, want, app_mqtt_from_broker,
                                              app_mqtt_state_changed, app, now_ms);
    if (started < 0) {
        inkwell_log_warn("mqtt", "Cannot proxy to %s: %d", want->address, started);
        return;
    }

    for (size_t i = 0U; i < count; ++i) {
        const int added = mesh_mqtt_proxy_subscribe(&app->mqtt, filters[i]);
        if (added < 0) {
            inkwell_log_warn("mqtt", "Cannot subscribe to %s: %d", filters[i], added);
        }
    }
    /* Only now, so a refused start leaves the radio's messages counted as unhandled rather than
       published into nothing. */
    mesh_session_set_mqtt_handler(&app->session, app_mqtt_from_radio, app);
    inkwell_log_info("mqtt", "Proxying for the radio to %s, %zu subscription%s", want->address,
                     count, count == 1U ? "" : "s");
}

void mesh_app_mqtt_init(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }
    app->mqtt_undelivered = 0U;
    memset(&app->mqtt_planned, 0, sizeof app->mqtt_planned);
    /*
     * A kill switch, on the same terms as MESHCLIENT_AUTOCONNECT.
     *
     * Everything this file decides is read off the *radio's* configuration, which means a radio the
     * user has just picked up can put this client on somebody's broker without anything being typed
     * on the Brick. That is the feature working as intended, and it is still worth being able to
     * say no to from a shell without editing a radio.
     */
    app->mqtt_disabled = !inkwell_env_bool("MQTT_PROXY", "MQTT client proxy", true);
    if (app->mqtt_disabled) {
        inkwell_log_info("app", "MQTT client proxy disabled by MESHCLIENT_MQTT_PROXY");
    }
    (void)mesh_mqtt_proxy_init(&app->mqtt, &app->loop);
    /* The built-in roots, unless somebody named a bundle. See include/mesh/core/ca_roots.h. */
    mesh_mqtt_proxy_set_ca_bundle(&app->mqtt, mesh_tls_ca_override());
}

void mesh_app_mqtt_shutdown(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }
    mesh_session_set_mqtt_handler(&app->session, NULL, NULL);
    mesh_mqtt_proxy_shutdown(&app->mqtt);
}

void mesh_app_mqtt_tick(struct mesh_app *app, uint64_t now_ms) {
    if (app == NULL) {
        return;
    }

    struct mesh_mqtt_proxy_config want;
    const bool wanted = !app->mqtt_disabled && mesh_app_mqtt_plan(&app->session, &want);

    if (!wanted) {
        if (app->mqtt_planned.active) {
            inkwell_log_info("mqtt", "No longer proxying for this radio");
            app_mqtt_stop(app);
        }
    } else {
        char filters[MESH_MQTT_FILTERS_MAX][MESH_MQTT_FILTER_MAX];
        const size_t count = mesh_app_mqtt_filters(&app->session, filters, MESH_MQTT_FILTERS_MAX);
        /*
         * A changed subscription set is a reconnect, not an added subscription, because the set
         * can *shrink*: mesh_mqtt_proxy_clear_filters() deliberately does not unsubscribe on the
         * wire, so a channel whose downlink the user just turned off would keep delivering until
         * the connection went away by itself. Dropping and remaking is the cheap option in
         * practice - every way this set can change is a settings write, and a settings write
         * reboots the radio and takes the link with it anyway.
         */
        if (mesh_app_mqtt_plan_changed(&app->mqtt_planned, &want, filters, count)) {
            app_mqtt_start(app, &want, filters, count, now_ms);
        }
    }

    /* Unconditionally, including while OFF: the proxy's own tick is what drives the resolver
       and the retry backoff, neither of which has a descriptor to be woken by. */
    mesh_mqtt_proxy_tick(&app->mqtt, now_ms);
}

void mesh_app_mqtt_publish_state(const struct mesh_app *app, struct mesh_ui_mqtt_state *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (app == NULL) {
        return;
    }

    /*
     * `wanted` is asked of the *radio*, not of the proxy, and that is the difference between a
     * card that can explain itself and one that cannot. A proxy sitting at OFF because the
     * client was told not to run one, and a proxy sitting at OFF because no radio ever asked,
     * are the same state and different problems; only the radio's configuration tells them
     * apart. Asking it again here rather than caching the last answer costs a few string
     * compares and means the card cannot disagree with the tick that acted on it.
     */
    struct mesh_mqtt_proxy_config want;
    out->wanted = mesh_app_mqtt_plan(&app->session, &want);
    if (!out->wanted) {
        return;
    }
    out->disabled = app->mqtt_disabled;

    const enum mesh_mqtt_proxy_state state = mesh_mqtt_proxy_state(&app->mqtt);
    (void)inkwell_str_copy(out->state, sizeof out->state, mesh_ui_mqtt_state_str(state));
    out->connected = state == MESH_MQTT_PROXY_READY;
    out->failing = state == MESH_MQTT_PROXY_WAITING;

    /*
     * The broker from the plan rather than from the proxy, so the row is answerable before
     * anything has connected and while the client is disabled - both of which are exactly when
     * somebody is reading this card. mesh_mqtt_proxy_host() is the parsed host and is empty
     * until a start, which would leave the one row that says *where* blank on the one screen
     * that exists to say why.
     */
    (void)inkwell_str_copy(out->host, sizeof out->host, want.address);
    mesh_ui_mqtt_failure_text(&app->mqtt, out->last_error, sizeof out->last_error);

    const struct mesh_mqtt_proxy_stats stats = mesh_mqtt_proxy_stats(&app->mqtt);
    out->published = stats.published;
    out->received = stats.received;
    out->dropped = stats.dropped;
    out->connections = stats.connections;
    /*
     * The subscriptions this client last *decided on*, not the ones the proxy holds. They are
     * the same number whenever anything is running, and they differ in the case the card is for:
     * a start the proxy refused leaves its own table empty while the plan still says what the
     * radio asked for, and "0 topics" there would blame the radio for the client's refusal.
     */
    out->subscriptions = (uint32_t)app->mqtt_planned.filter_count;
    out->undelivered = app->mqtt_undelivered;
    out->unhandled = app->session.mqtt_unhandled;
}
