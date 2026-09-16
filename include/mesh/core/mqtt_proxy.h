#pragma once

#include "mesh/core/event_loop.h"
#include "mesh/core/resolve.h"
#include "mesh/core/tls_client.h"

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One MQTT 3.1.1 connection to a broker, on behalf of a radio that cannot reach one itself.
 *
 * A Meshtastic radio with `proxy_to_client_enabled` set does not open its own MQTT connection.
 * It hands the client a `MqttClientProxyMessage` - a topic and a payload it has already decided
 * on - and expects the client to put it on a broker, and to hand back anything that arrives on
 * the topics it is interested in. The radio remains the origin; this is only the box with a
 * route to the internet. That is the entire contract, and it is why nothing in here understands
 * a service envelope or a channel: the topics are the radio's business (they arrive on the
 * message) and this module's business is the socket underneath them.
 *
 * **Everything is on the one epoll loop.** The socket is non-blocking, the name lookup is
 * mesh_resolve's forked child, and TLS is a state machine over the same descriptor. There is no
 * thread here and there is nowhere for one to go.
 *
 * **Nothing is queued across a disconnect.** A publish that arrives while the broker is
 * unreachable is dropped and counted, not held: what goes through here is position reports,
 * telemetry and chat, all of which are worth less the older they get, and replaying a minute of
 * them on reconnect would put a burst of stale readings on somebody's map. A partial *write* is
 * a different thing and is buffered - that is one packet mid-flight, not a backlog.
 */

/* The radio's own fields, which this cannot be smaller than: MQTTConfig.address is 64 bytes,
   username 64, password 32. Sized from the protobuf so a configuration that fits the radio
   always fits here. */
#define MESH_MQTT_ADDRESS_MAX 64U
#define MESH_MQTT_USERNAME_MAX 64U
#define MESH_MQTT_PASSWORD_MAX 32U
/* Long enough for "meshclient-!" and a 32-bit node id in hex, with room to spare. */
#define MESH_MQTT_CLIENT_ID_MAX 32U

/*
 * How many topic filters one connection may hold, and how long each may be.
 *
 * Eight because a radio has at most eight channels and each contributes one filter; the
 * direct-message topic makes nine, so the array is one larger than the channel count rather
 * than exactly it. A filter is longer than a topic - it carries the root, the region and a
 * wildcard - so it is not sized from the radio's 60-byte topic field.
 */
#define MESH_MQTT_FILTERS_MAX 9U
#define MESH_MQTT_FILTER_MAX 96U

/*
 * The largest MQTT packet this will assemble or accept.
 *
 * Bounded by what the radio can hold, not by what MQTT allows: a `MqttClientProxyMessage` has a
 * 60-byte topic and a 435-byte payload, so anything larger could not be handed on even if it
 * were read. A broker message past this is skipped - counted off the stream without being
 * buffered - which is what keeps the inbound buffer this size instead of the 256 MB a remaining
 * length can describe.
 */
#define MESH_MQTT_PACKET_MAX 1024U
/* Room for a packet mid-write plus the next one behind it. */
#define MESH_MQTT_OUTBOUND_MAX 4096U
/* `MqttClientProxyMessage.topic` is 60 bytes including its terminator, so a topic that does not
   fit here is one the radio could not be told about even if it were forwarded. */
#define MESH_MQTT_TOPIC_MAX 60U

/* Ports nobody writes down: 1883 is MQTT, 8883 is MQTT over TLS. A target that names its own
   port overrides both. */
#define MESH_MQTT_PORT 1883U
#define MESH_MQTT_PORT_TLS 8883U

/*
 * How long a quiet connection waits before saying something, and before giving up on the answer.
 *
 * The keepalive is what the broker is told; it disconnects a client that has been silent for
 * one and a half times it. The ping interval is comfortably inside that, and the silence
 * deadline is what notices the other direction - a broker that went away without closing
 * anything, which on a handheld carried out of WiFi range is the ordinary case rather than the
 * exotic one.
 */
#define MESH_MQTT_KEEPALIVE_S 60U
#define MESH_MQTT_PING_INTERVAL_MS 30000U
#define MESH_MQTT_SILENCE_TIMEOUT_MS 90000U
/* A connect, a TLS handshake and a CONNACK each get their own deadline; none of them is allowed
   to leave the state machine parked forever on a socket that will never answer. */
#define MESH_MQTT_CONNECT_TIMEOUT_MS 10000U
#define MESH_MQTT_HANDSHAKE_TIMEOUT_MS 15000U

/*
 * Backoff between attempts: doubling from five seconds, capped at five minutes.
 *
 * Slower than the radio link's, deliberately. A radio that is out of range comes back when the
 * user walks back into the room and a fast retry is what makes that feel instant; a broker that
 * is refusing us is usually refusing us for a reason that will still be true in a second, and
 * the far end is somebody else's server. Five minutes is the longest a person would plausibly
 * wait without giving up and toggling the setting, which is the manual override.
 */
#define MESH_MQTT_BACKOFF_BASE_MS 5000U
#define MESH_MQTT_BACKOFF_MAX_MS 300000U

/*
 * Where the connection is, which is also what the UI says about it.
 *
 * One state per distinct sentence, so a status row is a table lookup rather than a chain of
 * conditions over a bag of booleans - and so that "connecting" and "connected but the broker has
 * not accepted us" cannot be confused, which is the difference between a broker that is
 * unreachable and one that is rejecting the password.
 */
enum mesh_mqtt_proxy_state {
    MESH_MQTT_PROXY_OFF = 0, /* not asked to run */
    MESH_MQTT_PROXY_RESOLVING,
    MESH_MQTT_PROXY_CONNECTING, /* TCP handshake */
    MESH_MQTT_PROXY_SECURING,   /* TLS handshake */
    MESH_MQTT_PROXY_GREETING,   /* CONNECT is on the wire, CONNACK has not come back */
    MESH_MQTT_PROXY_READY,      /* the broker accepted us */
    MESH_MQTT_PROXY_WAITING,    /* an attempt failed; the next one is scheduled */
    MESH_MQTT_PROXY_STATE_COUNT,
};

/* What to connect to. Copied on start(), so the caller's buffer need not outlive the call. */
struct mesh_mqtt_proxy_config {
    /* "host", "host:port", or "[v6]:port". An empty address is refused rather than defaulted:
       the radio's own default belongs to whoever reads the radio's configuration, not here. */
    char address[MESH_MQTT_ADDRESS_MAX];
    char username[MESH_MQTT_USERNAME_MAX];
    char password[MESH_MQTT_PASSWORD_MAX];
    /*
     * Must be unique on the broker. Two clients presenting the same id is not an error a broker
     * reports - it disconnects the older one - so two Bricks proxying for two radios with the
     * same id would take turns kicking each other off, reconnecting, and looking to each user
     * like a broker that flaps. Derive it from the radio's node id.
     */
    char client_id[MESH_MQTT_CLIENT_ID_MAX];
    bool tls_enabled;
};

/*
 * A message from the broker, on a topic this connection subscribed to. `topic` is
 * NUL-terminated and `payload` is not; both are only valid for the duration of the call.
 *
 * Called from the event loop, never from publish() or start().
 */
typedef void (*mesh_mqtt_proxy_message_fn)(void *userdata, const char *topic,
                                           const uint8_t *payload, size_t len);

/* Told once per transition, for a UI that wants to react rather than poll. May be NULL. */
typedef void (*mesh_mqtt_proxy_state_fn)(void *userdata, enum mesh_mqtt_proxy_state state);

/*
 * What has gone through this proxy, for the status screen. Counted for the life of the proxy
 * rather than of one connection - `connections` is what makes a link that keeps dropping and
 * remaking itself visible, and it could not do that if a reconnection cleared the rest.
 */
struct mesh_mqtt_proxy_stats {
    uint32_t published;   /* publishes handed to the socket */
    uint32_t received;    /* messages delivered to the callback */
    uint32_t dropped;     /* publishes refused: not connected, too large, or no room */
    uint32_t skipped;     /* inbound messages too large to forward, counted off the stream */
    uint32_t connections; /* successful CONNACKs, so a flapping link is visible as one */
};

struct mesh_mqtt_proxy {
    struct mesh_event_loop *loop;
    enum mesh_mqtt_proxy_state state;
    struct mesh_mqtt_proxy_config config;

    /* The host and port taken out of config.address once, at start. */
    char host[MESH_MQTT_ADDRESS_MAX];
    uint16_t port;

    struct mesh_resolve resolve;
    int fd;
    bool fd_registered;
    bool want_write; /* EPOLLOUT is armed because something is waiting to go out */

    /* Only started when config.tls_enabled; a plaintext connection never touches it, and
       `tls.state` being non-NULL is what every read and write branches on. */
    struct mesh_tls_client tls;
    /* Resolved by somebody else and handed over; see mesh_mqtt_proxy_set_ca_bundle(). */
    char ca_bundle[256];

    uint8_t in[MESH_MQTT_PACKET_MAX];
    size_t in_len;
    /*
     * The socket was still readable when the per-turn read budget ran out - or, under TLS,
     * plaintext is already decrypted and sitting inside the session where epoll cannot see it
     * and will never report it again. Either way the next tick() has to come back and read
     * rather than wait to be woken.
     */
    bool more_to_read;
    /*
     * A TLS read blocked on *writability* rather than on more bytes arriving.
     *
     * The two directions come apart under TLS: `mbedtls_ssl_read()` may have to send something
     * before it can return anything - refusing a renegotiation with an alert is the reachable
     * case, since this client never enables renegotiation and a TLS 1.2 broker is free to ask -
     * and on a full socket that send is what reports WANT_WRITE. Waiting for EPOLLIN then waits
     * for the wrong event: the peer is not going to speak again until we have spoken.
     *
     * Kept apart from `tls.wants_write`, which belongs to whichever call blocked last and is
     * cleared by the next one that succeeds. A write finishing must not retract a read's claim
     * on EPOLLOUT.
     */
    bool read_wants_write;
    /* Bytes of an oversized inbound body still to be read and discarded. The stream stays in
       sync because these are counted off it; nothing is ever re-synchronised by guessing. */
    size_t skip_remaining;

    uint8_t out[MESH_MQTT_OUTBOUND_MAX];
    size_t out_len;
    size_t out_sent; /* cursor into out[], for a write the socket only took part of */
    /*
     * The length handed to a write that could not finish, to be retried verbatim.
     *
     * A TLS requirement rather than a socket one: `mbedtls_ssl_write()` that returns WANT_WRITE
     * has committed to a record of a particular length and must be called again with the same
     * arguments. Coming back with a longer buffer - because a publish was queued in between -
     * is undefined behaviour in the library.
     */
    size_t out_pending;

    char filters[MESH_MQTT_FILTERS_MAX][MESH_MQTT_FILTER_MAX];
    size_t filter_count;
    /* Which filter is waiting for its SUBACK, and under what id. Subscriptions go out one at a
       time so a refusal can be attributed to the filter that caused it. */
    size_t filter_sent;
    uint16_t subscribe_id;

    /*
     * The clock, set by tick() and read by everything else.
     *
     * Half of what this module times is *set* from an fd callback and *compared* in tick(): a
     * deadline armed when a socket connects, a retry armed when one fails. Reading the real
     * clock at each of those points would be two different numbers, and would leave a caller
     * driving a synthetic clock - every test here - comparing its own time against the
     * machine's. One source, slightly stale inside a callback, monotonic either way.
     */
    uint64_t now_ms;
    uint64_t deadline_ms; /* the current state's, or 0 when it has none */
    uint64_t next_ping_ms;
    uint64_t last_heard_ms;
    uint64_t retry_at_ms;
    uint32_t failures; /* consecutive, for the backoff; cleared by a CONNACK */

    struct mesh_mqtt_proxy_stats stats;
    /* Why the last attempt failed, in words, for the status screen. Empty until one does. */
    char last_error[128];

    mesh_mqtt_proxy_message_fn on_message;
    mesh_mqtt_proxy_state_fn on_state;
    void *userdata;
};

/*
 * Prepares a proxy that is not running. `loop` may be NULL, which leaves it permanently OFF -
 * what a caller that has no loop, and a test that means to spawn nothing, both want.
 * Returns 0, or -EINVAL.
 */
int mesh_mqtt_proxy_init(struct mesh_mqtt_proxy *proxy, struct mesh_event_loop *loop);

/* Drops any connection without reporting it and releases everything held. */
void mesh_mqtt_proxy_shutdown(struct mesh_mqtt_proxy *proxy);

/*
 * The CA bundle every TLS connection verifies against.
 *
 * Takes a path somebody else resolved rather than resolving its own:
 * `mesh_fetch_resolve_ca_bundle()` already works this out - an environment override, then the
 * bundle shipped inside our own pak, then the system locations - and the answer is a fact about how
 * this binary was installed. Two modules working it out separately is two answers that can
 * disagree. Ignored without TLS.
 */
void mesh_mqtt_proxy_set_ca_bundle(struct mesh_mqtt_proxy *proxy, const char *path);

/*
 * Points the proxy at a broker and starts trying to reach it.
 *
 * A successful call replaces whatever it was doing, including an established connection: a
 * changed address is a different broker. A refused one changes nothing - the previous connection
 * is left alone rather than torn down over a configuration that could not be used anyway.
 *
 * `on_message` may be NULL, which subscribes to nothing usefully but is legal. Returns 0, or
 * -ENOTSUP with no loop, -EINVAL for an address that is not a host or a missing client id. On an
 * error nothing was opened.
 *
 * Returning 0 means the attempt is under way, not that it worked; watch the state.
 */
int mesh_mqtt_proxy_start(struct mesh_mqtt_proxy *proxy,
                          const struct mesh_mqtt_proxy_config *config,
                          mesh_mqtt_proxy_message_fn on_message, mesh_mqtt_proxy_state_fn on_state,
                          void *userdata, uint64_t now_ms);

/*
 * Disconnects and goes to OFF. A connected proxy sends a DISCONNECT first, which is what tells
 * the broker this was deliberate - without it a client that goes quiet looks to the broker like
 * one that crashed, and a broker with a will message set would act on it.
 */
void mesh_mqtt_proxy_stop(struct mesh_mqtt_proxy *proxy);

/*
 * Adds a topic filter. Subscribed immediately when the connection is up, and on every
 * reconnection afterwards - a broker with a clean session remembers nothing, so the set has to
 * be re-sent each time rather than assumed.
 *
 * Returns 0, -EEXIST when the filter is already held (which is not a failure and is how a caller
 * that re-derives the set every config change stays idempotent), -ENOSPC when the table is full,
 * or -EINVAL for a filter that is empty or too long.
 */
int mesh_mqtt_proxy_subscribe(struct mesh_mqtt_proxy *proxy, const char *filter);

/* Empties the filter table. Does not unsubscribe on the wire: the only caller is about to
   re-derive the whole set, and the connection is dropped and remade around it. */
void mesh_mqtt_proxy_clear_filters(struct mesh_mqtt_proxy *proxy);

/*
 * Puts one message on the broker, at QoS 0.
 *
 * Returns 0, or -ENOTCONN when the broker has not accepted us - which is the ordinary answer
 * while a link is down and is counted, not logged, because the radio will keep offering. Also
 * -EINVAL for a topic that is empty or carries a wildcard, -EMSGSIZE for a message larger than
 * this will assemble, and -ENOSPC when the outbound buffer has not drained. Every one of those
 * increments `dropped`.
 */
int mesh_mqtt_proxy_publish(struct mesh_mqtt_proxy *proxy, const char *topic,
                            const uint8_t *payload, size_t len, bool retained);

/*
 * Drives the deadlines, the keepalive and the backoff. Call every loop turn.
 *
 * The fd callback does the reading and writing; this does everything that is a clock rather than
 * a descriptor, including the retry that has no descriptor to be woken by.
 */
void mesh_mqtt_proxy_tick(struct mesh_mqtt_proxy *proxy, uint64_t now_ms);

enum mesh_mqtt_proxy_state mesh_mqtt_proxy_state(const struct mesh_mqtt_proxy *proxy);
/* True only in READY: the one state in which a publish will be taken. */
bool mesh_mqtt_proxy_is_ready(const struct mesh_mqtt_proxy *proxy);
struct mesh_mqtt_proxy_stats mesh_mqtt_proxy_stats(const struct mesh_mqtt_proxy *proxy);
/* Why the last attempt failed, or "" when none has. Never NULL. */
const char *mesh_mqtt_proxy_last_error(const struct mesh_mqtt_proxy *proxy);
/* The broker being talked to, for a status row. Never NULL; "" before a start. */
const char *mesh_mqtt_proxy_host(const struct mesh_mqtt_proxy *proxy);
/*
 * The state as a sentence, from the string catalog. Here rather than in a UI table because it
 * is the same mapping the transports keep next to their own state enums - a screen naming the
 * id itself would have to enumerate these states a second time, and the two would drift.
 */
const char *mesh_mqtt_proxy_state_string(enum mesh_mqtt_proxy_state state);

#ifdef __cplusplus
}
#endif
