#pragma once

/*
 * The broker connection this client holds on behalf of the attached radio.
 *
 * Its own record, and its own subject, because it is the one thing on this client that is a
 * *second link*. Everything else in the store describes the radio, the mesh, or what the user is
 * doing; this describes a socket to the internet that exists only because a radio asked for one,
 * and whose failures have nothing to do with the radio at all. A broker refusing a password and
 * a radio walking out of range are different problems with different answers, and a reader who
 * cannot tell them apart is exactly the reader this record exists for.
 *
 * Not persisted. It is true of a connection that is up now; a cache that restored "connected"
 * from last Tuesday would be the one lie a diagnostic screen must not tell.
 *
 * See mesh/ui/store.h for the whole.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The broker's name as it will be drawn: MESH_MQTT_ADDRESS_MAX said again on this side of the
 * seam, which is the arrangement MESH_UI_NETWORK_HOST_MAX already has and for the same reason -
 * this header names no core module, and pulling mesh/core/mqtt_proxy.h in to reach one number
 * would drag the TLS client and the resolver into every screen that draws a card.
 * mqtt_status_limits_agree_across_the_seam holds the two honest.
 */
#define MESH_UI_MQTT_HOST_MAX 64U
/* MESH_MQTT_PROXY_STATE_COUNT sentences, the longest of which is "Securing the connection". */
#define MESH_UI_MQTT_STATE_MAX 48U
/* mesh_mqtt_proxy.last_error, which is a whole sentence naming a host and a reason. */
#define MESH_UI_MQTT_ERROR_MAX 128U

struct mesh_ui_mqtt_state {
    /*
     * The radio asked to be proxied for - `MQTTConfig.enabled` and `proxy_to_client_enabled`
     * together, once the config sync has finished.
     *
     * The whole card is drawn on this. A radio that reaches its own broker, or none at all, has
     * nothing for this client to report, and a card saying "Off" on every Brick in the world
     * would be four rows of screen spent on a feature almost nobody has turned on.
     */
    bool wanted;
    /*
     * The client declined, which is MESHCLIENT_MQTT_PROXY=0 and nothing else.
     *
     * Apart from `wanted` because the two are different sentences and only one of them is
     * something the reader can do anything about. "Your radio is not asking" sends them to the
     * radio; "this client was told not to" sends them to launch.sh.
     */
    bool disabled;
    /*
     * Where the connection is, as a sentence the core's own table produced.
     *
     * Resolved rather than published as an id, the way `transport_status` is: the mapping from
     * state to words lives next to the enum in mesh/core/mqtt_proxy.h, on the argument that a
     * screen naming the ids itself would enumerate those states a second time and the two would
     * drift. Carrying the words across the seam is what lets that stay true.
     */
    char state[MESH_UI_MQTT_STATE_MAX];
    /*
     * Which of the states it is in, for the two that need a colour rather than a sentence.
     *
     * Two booleans rather than the enum, deliberately. A renderer choosing a tone has three
     * cases - it worked, it failed, it is somewhere in between - and the seven states collapse
     * onto those three. Publishing the enum would put the seven on this side as well, where the
     * only thing anybody would ever do with them is collapse them again.
     */
    bool connected; /* the broker accepted us */
    bool failing;   /* an attempt failed; another is scheduled */
    /*
     * The broker being talked to, which is worth a row of its own because the radio may never
     * have named it: an empty `MQTTConfig.address` means the public broker, and that
     * substitution happens here rather than on the radio. A reader looking at the Settings
     * screen sees a blank Server field and no way to tell what it resolved to.
     */
    char host[MESH_UI_MQTT_HOST_MAX];
    /* Why the last attempt failed, or "" when none has. What the whole card is for. */
    char last_error[MESH_UI_MQTT_ERROR_MAX];
    /* How many topics are subscribed to. Zero is the answer to "why does nothing arrive" on a
       radio whose channels all have downlink off, which is otherwise invisible from here. */
    uint32_t subscriptions;
    uint32_t published;
    uint32_t received;
    /* Publishes refused: not connected, too large, or no room. The radio keeps offering while a
       broker is unreachable, so on a link that is down this is the number that moves. */
    uint32_t dropped;
    /* Successful sign-ins over the life of the proxy, so a connection that keeps dropping and
       remaking itself is visible as something other than "Connected". */
    uint32_t connections;
    /* Broker messages that reached the radio's doorstep and no further - the link dropping
       mid-turn, or a message larger than the radio's own 435-byte field. */
    uint32_t undelivered;
    /* Messages the radio offered with nowhere to put them, which is what `disabled` looks like
       from the radio's side and is counted for the current link only. */
    uint32_t unhandled;
};

#ifdef __cplusplus
}
#endif
