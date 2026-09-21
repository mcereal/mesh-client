#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mesh/core/mqtt_proxy.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"

#include "inkwell/codec/mqtt.h"
#include "inkwell/net/reason.h"
#include "mesh/transport/tcp.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * How many reads one readiness event may take before giving the loop back.
 *
 * The same reasoning as mesh_stream_link_pump()'s bound: a busy broker - and the public one
 * carries the whole of somebody's mesh - can hand over data faster than this can be woken to ask
 * for it, and a loop that reads until EAGAIN would keep the UI from drawing for as long as that
 * lasts. Stopping early is safe because `more_to_read` remembers that we did.
 */
#define MQTT_READS_PER_TURN 8U

/* The shift cap for the backoff, so the doubling cannot overflow before the clamp catches it. */
#define MQTT_BACKOFF_SHIFT_MAX 6U

static void mqtt_close(struct mesh_mqtt_proxy *proxy);
static void mqtt_attempt(struct mesh_mqtt_proxy *proxy);
static int mqtt_fd_callback(int fd, uint32_t events, void *userdata);

/* ------------------------------------------------------------------ small things */

static void mqtt_set_state(struct mesh_mqtt_proxy *proxy, enum mesh_mqtt_proxy_state state) {
    if (proxy->state == state) {
        return;
    }
    proxy->state = state;
    if (proxy->on_state != NULL) {
        proxy->on_state(proxy->userdata, state);
    }
}

/*
 * Ends the attempt, says why, and schedules the next one.
 *
 * Every failure path goes through here rather than closing the socket itself, which is what
 * guarantees the two things that must always happen together actually do: the descriptor is
 * released *and* a retry is armed. A close without a retry is a proxy that is silently off with
 * the setting still on, which is the failure this shape exists to make impossible.
 *
 * The first reason wins until it is read. A failure cascades - a TLS error becomes a dropped
 * socket becomes a closed connection - and the first of the three is the only one that explains
 * anything.
 */
static bool mqtt_has_failure(const struct mesh_mqtt_proxy *proxy) {
    return inkwell_net_failed(&proxy->failure.net) ||
           proxy->failure.refusal != MESH_MQTT_REFUSAL_NONE;
}

/* A short ASCII name for a refusal, for a log line and for nothing else. The same kind of thing
   as inkwell_net_reason_name(), and not fit for a screen for the same reason. */
static const char *mqtt_refusal_name(enum mesh_mqtt_refusal refusal) {
    switch (refusal) {
    case MESH_MQTT_REFUSAL_BAD_ADDRESS:
        return "bad-address";
    case MESH_MQTT_REFUSAL_NO_TLS:
        return "no-tls";
    case MESH_MQTT_REFUSAL_PROTOCOL:
        return "not-mqtt";
    case MESH_MQTT_REFUSAL_BAD_LOGIN:
        return "bad-login";
    case MESH_MQTT_REFUSAL_NOT_ALLOWED:
        return "not-allowed";
    case MESH_MQTT_REFUSAL_BROKER_BUSY:
        return "broker-busy";
    case MESH_MQTT_REFUSAL_OTHER:
        return "refused";
    case MESH_MQTT_REFUSAL_NONE:
    case MESH_MQTT_REFUSAL_COUNT:
    default:
        break;
    }
    return "unknown";
}

/*
 * Close, count, arm the retry, and say so in the log.
 *
 * `why` and `detail` are the log line and nothing else: they are ASCII, they are English, and
 * neither comes out of the string catalog. That is deliberate rather than an oversight to fix
 * later - this line used to print the translated sentence, so a Spanish device wrote its
 * retry loop in Spanish and a bug report nobody could read. What a *reader* is told is built
 * from the record, elsewhere, when a screen asks.
 */
static void mqtt_back_off(struct mesh_mqtt_proxy *proxy, const char *why, const char *detail) {
    mqtt_close(proxy);

    if (proxy->failures < UINT32_MAX) {
        proxy->failures++;
    }
    const unsigned shift =
        proxy->failures > 1U
            ? (unsigned)(proxy->failures - 1U > MQTT_BACKOFF_SHIFT_MAX ? MQTT_BACKOFF_SHIFT_MAX
                                                                       : proxy->failures - 1U)
            : 0U;
    uint64_t delay = (uint64_t)MESH_MQTT_BACKOFF_BASE_MS << shift;
    if (delay > MESH_MQTT_BACKOFF_MAX_MS) {
        delay = MESH_MQTT_BACKOFF_MAX_MS;
    }
    proxy->retry_at_ms = proxy->now_ms + delay;
    mqtt_set_state(proxy, MESH_MQTT_PROXY_WAITING);
    /* The address rather than the host when the host was never parsed out of it, which is
       exactly the case a bad address is. */
    const char *const subject = proxy->host[0] != '\0' ? proxy->host : proxy->config.address;
    inkwell_log_warn("mqtt", "%s: %s%s%s; retrying in %llums", subject, why,
                     detail[0] != '\0' ? ": " : "", detail, (unsigned long long)delay);
}

/* A failure any link has: inkwell's reason, and the number behind it. `detail` is the negative
   errno for UNREACHABLE, the EAI_* for a lookup, and 0 where the reason says everything. */
static void mqtt_fail_net(struct mesh_mqtt_proxy *proxy, enum inkwell_net_reason reason,
                          int detail) {
    const bool first = !mqtt_has_failure(proxy);
    if (first) {
        proxy->failure.net.reason = reason;
        proxy->failure.net.detail = detail;
    }
    mqtt_back_off(proxy, inkwell_net_reason_name(reason),
                  reason == INKWELL_NET_UNREACHABLE ? strerror(-detail) : "");
}

/* A failure this proxy or the broker named. `code` is the CONNACK byte and is read only for
   MESH_MQTT_REFUSAL_OTHER. */
static void mqtt_fail_own(struct mesh_mqtt_proxy *proxy, enum mesh_mqtt_refusal refusal,
                          uint8_t code) {
    if (!mqtt_has_failure(proxy)) {
        proxy->failure.refusal = refusal;
        proxy->failure.code = code;
    }
    mqtt_back_off(proxy, mqtt_refusal_name(refusal), "");
}

/* TLS gets its own entry point because the only account of what went wrong is the library's
   own sentence, which is already sitting in the session and cannot be rebuilt from a number. */
static void mqtt_fail_tls(struct mesh_mqtt_proxy *proxy) {
    if (!mqtt_has_failure(proxy)) {
        proxy->failure.net.reason = INKWELL_NET_TLS;
        proxy->failure.net.detail = 0;
    }
    mqtt_back_off(proxy, inkwell_net_reason_name(INKWELL_NET_TLS),
                  mesh_tls_client_error(&proxy->tls));
}

/* ------------------------------------------------------------------ the descriptor */

/*
 * Which epoll events this connection wants right now.
 *
 * Recomputed from the state rather than toggled, because the answer has three independent
 * sources - a connect in flight, a write queue with a remainder, and a TLS session blocked on
 * writability - and any two of them can be true at once. Toggling would mean every one of the
 * three had to know what the other two wanted before clearing anything.
 */
static void mqtt_arm(struct mesh_mqtt_proxy *proxy) {
    if (proxy->fd < 0 || !proxy->fd_registered || proxy->loop == NULL) {
        return;
    }
    bool want_write = proxy->state == MESH_MQTT_PROXY_CONNECTING;
    if (proxy->out_sent < proxy->out_len) {
        want_write = true;
    }
    if (proxy->tls.state != NULL && (proxy->tls.wants_write || proxy->read_wants_write)) {
        want_write = true;
    }
    if (want_write == proxy->want_write) {
        return;
    }
    proxy->want_write = want_write;
    const uint32_t events = (uint32_t)EPOLLIN | (want_write ? (uint32_t)EPOLLOUT : 0U);
    (void)inkwell_loop_update_fd(proxy->loop, proxy->fd, events);
}

/*
 * Reading and writing, with the one decision about *how* made in one place each.
 *
 * Two functions rather than a table of function pointers. There are exactly two implementations,
 * both are compiled into every build that has TLS at all, and neither is ever chosen per call -
 * it is settled for the whole connection at start. A vtable here would buy the ability to
 * substitute a third, which nothing wants: the tests drive a real socket against a real broker
 * on loopback, which exercises far more of this than a scripted fake would.
 */
static int mqtt_raw_read(struct mesh_mqtt_proxy *proxy, uint8_t *out, size_t cap) {
    if (proxy->tls.state != NULL) {
        return mesh_tls_client_read(&proxy->tls, out, cap);
    }
    const ssize_t got = recv(proxy->fd, out, cap, 0);
    if (got > 0) {
        return (int)got;
    }
    if (got == 0) {
        return -ENOTCONN;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return -EAGAIN;
    }
    return -errno;
}

static int mqtt_raw_write(struct mesh_mqtt_proxy *proxy, const uint8_t *data, size_t len) {
    if (proxy->tls.state != NULL) {
        return mesh_tls_client_write(&proxy->tls, data, len);
    }
    /* MSG_NOSIGNAL for the reason stream_link.h gives: a write to a socket whose peer has gone
       raises SIGPIPE, and its default disposition would kill the client outright. */
    const ssize_t written = send(proxy->fd, data, len, MSG_NOSIGNAL);
    if (written >= 0) {
        return (int)written;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return -EAGAIN;
    }
    return -errno;
}

/* ------------------------------------------------------------------ the write queue */

/* Moves the unsent remainder to the front, so a long-lived connection does not run out of
   buffer having sent everything in it. */
static void mqtt_compact_out(struct mesh_mqtt_proxy *proxy) {
    if (proxy->out_sent == 0U) {
        return;
    }
    /* Never while a TLS write is mid-record: the bytes it is retrying have to stay exactly where
       they were. See `out_pending` below. */
    if (proxy->out_pending > 0U) {
        return;
    }
    const size_t left = proxy->out_len - proxy->out_sent;
    if (left > 0U) {
        memmove(proxy->out, proxy->out + proxy->out_sent, left);
    }
    proxy->out_len = left;
    proxy->out_sent = 0U;
}

/*
 * Pushes what is queued at the socket. Returns 0, or a negative errno that is fatal to the
 * connection.
 *
 * `out_pending` is the subtle part and it is a TLS requirement rather than a socket one. When
 * `mbedtls_ssl_write()` cannot finish, it has already committed to a record of a particular
 * length and demands the same buffer and the same length next time; coming back with a longer
 * one - because a publish was queued in between - is undefined. So the length attempted is
 * remembered across the block and reused verbatim. On a plain socket it costs one comparison.
 */
static int mqtt_flush(struct mesh_mqtt_proxy *proxy) {
    bool sent_something = false;

    while (proxy->out_sent < proxy->out_len) {
        size_t chunk = proxy->out_len - proxy->out_sent;
        if (proxy->out_pending > 0U) {
            chunk = proxy->out_pending;
        }
        const int written = mqtt_raw_write(proxy, proxy->out + proxy->out_sent, chunk);
        if (written == -EAGAIN) {
            proxy->out_pending = chunk;
            break;
        }
        if (written < 0) {
            return written;
        }
        proxy->out_pending = 0U;
        proxy->out_sent += (size_t)written;
        sent_something = true;
    }
    if (proxy->out_sent >= proxy->out_len) {
        proxy->out_len = 0U;
        proxy->out_sent = 0U;
        proxy->out_pending = 0U;
    }
    /*
     * Bytes that actually went out are traffic the broker has heard from us, and a PINGREQ is
     * only needed when nothing else has been said - so the keepalive is pushed back here rather
     * than on the clock.
     *
     * Only when something moved, deliberately. A flush that wrote nothing has told the broker
     * nothing, and treating it as if it had would let a connection that is merely writable keep
     * postponing the one packet that proves it is still there.
     */
    if (sent_something) {
        proxy->next_ping_ms = proxy->now_ms + MESH_MQTT_PING_INTERVAL_MS;
    }
    mqtt_arm(proxy);
    return 0;
}

/* Queues a whole packet and tries to send it. -ENOSPC when there is no room, which is a dropped
   message rather than a broken connection. */
static int mqtt_queue(struct mesh_mqtt_proxy *proxy, const uint8_t *data, size_t len) {
    if (proxy->out_len + len > sizeof proxy->out) {
        mqtt_compact_out(proxy);
    }
    if (proxy->out_len + len > sizeof proxy->out) {
        return -ENOSPC;
    }
    memcpy(proxy->out + proxy->out_len, data, len);
    proxy->out_len += len;
    return mqtt_flush(proxy);
}

/* ------------------------------------------------------------------ subscriptions */

/*
 * Sends the next filter that has not been acknowledged, if none is already in flight.
 *
 * One at a time, rather than all of them in a single SUBSCRIBE. A broker answers a multi-filter
 * SUBSCRIBE with one return code per filter in order, so attributing a refusal means counting -
 * and the thing a refusal has to say is *which topic*, since that is the channel whose traffic
 * will silently never arrive. One filter per packet makes that a lookup.
 */
static void mqtt_pump_subscribes(struct mesh_mqtt_proxy *proxy) {
    if (proxy->state != MESH_MQTT_PROXY_READY || proxy->subscribe_id != 0U) {
        return;
    }
    if (proxy->filter_sent >= proxy->filter_count) {
        return;
    }
    /* The index plus one: unique among what is in flight (there is only ever one) and readable
       in a packet capture next to the filter it belongs to. */
    const uint16_t id = (uint16_t)(proxy->filter_sent + 1U);
    uint8_t packet[MESH_MQTT_FILTER_MAX + 16U];
    const int len = inkwell_mqtt_encode_subscribe(packet, sizeof packet, id,
                                                  proxy->filters[proxy->filter_sent]);
    if (len < 0) {
        inkwell_log_warn("mqtt", "Cannot subscribe to %s: %d", proxy->filters[proxy->filter_sent],
                         len);
        proxy->filter_sent++;
        mqtt_pump_subscribes(proxy);
        return;
    }
    const int queued = mqtt_queue(proxy, packet, (size_t)len);
    if (queued == -ENOSPC) {
        /* A full buffer, not a dead socket. Not retried here: the next SUBACK or the next
           reconnect comes back through this function, and a buffer this full on a connection
           this new is already odd. */
        inkwell_log_warn("mqtt", "No room to send a subscription");
        return;
    }
    if (queued < 0) {
        mqtt_fail_net(proxy, INKWELL_NET_CLOSED, 0);
        return;
    }
    proxy->subscribe_id = id;
}

/* ------------------------------------------------------------------ inbound packets */

static bool mqtt_on_connack(struct mesh_mqtt_proxy *proxy, const uint8_t *body, size_t len) {
    if (proxy->state != MESH_MQTT_PROXY_GREETING) {
        mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_PROTOCOL, 0U);
        return false;
    }
    uint8_t code = 0U;
    if (inkwell_mqtt_decode_connack(body, len, &code, NULL) != 0) {
        mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_PROTOCOL, 0U);
        return false;
    }
    if (code != INKWELL_MQTT_CONNACK_ACCEPTED) {
        /*
         * Told apart because they are three different things for the user to do. A wrong
         * password is a setting on the radio; a client that is not permitted is an account on
         * the broker; a broker that is unavailable is somebody else's outage and waiting is the
         * only move. The rest are protocol-level and carry the number, which is all anyone
         * could act on anyway.
         */
        switch (code) {
        case INKWELL_MQTT_CONNACK_BAD_CREDENTIALS:
            mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_BAD_LOGIN, code);
            break;
        case INKWELL_MQTT_CONNACK_NOT_AUTHORISED:
            mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_NOT_ALLOWED, code);
            break;
        case INKWELL_MQTT_CONNACK_UNAVAILABLE:
            mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_BROKER_BUSY, code);
            break;
        default:
            mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_OTHER, code);
            break;
        }
        return false;
    }

    /*
     * Connected. The failure count is cleared here and nowhere earlier: a socket that opens and
     * then gets refused is not progress, and resetting the backoff on the TCP connect would turn
     * a broker that rejects our password into a five-second retry loop against somebody else's
     * server, forever.
     */
    proxy->failures = 0U;
    proxy->deadline_ms = 0U;
    proxy->stats.connections++;
    memset(&proxy->failure, 0, sizeof proxy->failure);
    mqtt_set_state(proxy, MESH_MQTT_PROXY_READY);
    inkwell_log_info("mqtt", "Connected to %s as %s", proxy->host, proxy->config.client_id);

    /* A clean session means the broker remembers no subscriptions, so the whole set goes out
       again on every reconnection rather than only on the first. */
    proxy->filter_sent = 0U;
    proxy->subscribe_id = 0U;
    mqtt_pump_subscribes(proxy);
    return true;
}

static bool mqtt_on_suback(struct mesh_mqtt_proxy *proxy, const uint8_t *body, size_t len) {
    uint16_t id = 0U;
    uint8_t code = 0U;
    if (inkwell_mqtt_decode_suback(body, len, &id, &code) != 0) {
        mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_PROTOCOL, 0U);
        return false;
    }
    if (id != proxy->subscribe_id) {
        /* Not ours, or a duplicate. Ignored rather than fatal: nothing about the connection is
           wrong, and dropping it would cost every other subscription too. */
        inkwell_log_debug("mqtt", "Unexpected SUBACK %u", (unsigned)id);
        return true;
    }
    if (code == INKWELL_MQTT_SUBACK_FAILURE) {
        /*
         * One refused filter, not a refused connection. The broker's ACLs may permit some
         * topics and not others, and dropping the link would lose the ones that work - but this
         * is worth saying out loud, because the symptom is a channel whose traffic simply never
         * arrives and nothing else would ever mention it.
         */
        inkwell_log_warn("mqtt", "%s refused the subscription to %s", proxy->host,
                         proxy->filters[proxy->filter_sent]);
    }
    proxy->filter_sent++;
    proxy->subscribe_id = 0U;
    mqtt_pump_subscribes(proxy);
    return true;
}

static bool mqtt_on_publish(struct mesh_mqtt_proxy *proxy, uint8_t flags, const uint8_t *body,
                            size_t len) {
    struct inkwell_mqtt_incoming message;
    if (inkwell_mqtt_decode_publish(flags, body, len, &message) != 0) {
        mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_PROTOCOL, 0U);
        return false;
    }
    /*
     * QoS 1 and 2 are decoded but not acknowledged. The subscription asked for 0, so a broker
     * sending more is doing something it was not asked to; honouring it properly would mean a
     * PUBACK ledger for messages whose value has expired by the time the radio hears them. The
     * message itself is still forwarded - it is real traffic and dropping it would be worse.
     */
    if (proxy->on_message == NULL) {
        return true;
    }
    /* The radio's own topic field is 60 bytes, so anything longer could not be handed on even
       if it were forwarded - and a truncated topic is worse than none, because the radio would
       act on the wrong channel. */
    char topic[MESH_MQTT_TOPIC_MAX];
    if (message.topic_len >= sizeof topic) {
        proxy->stats.skipped++;
        inkwell_log_debug("mqtt", "Dropped a message on a topic too long to forward (%zu bytes)",
                          message.topic_len);
        return true;
    }
    memcpy(topic, message.topic, message.topic_len);
    topic[message.topic_len] = '\0';

    proxy->stats.received++;
    proxy->on_message(proxy->userdata, topic, message.payload, message.payload_len);
    return true;
}

/* Handles one whole packet. False means the connection is gone and the buffers with it. */
static bool mqtt_handle(struct mesh_mqtt_proxy *proxy, const struct inkwell_mqtt_header *header,
                        const uint8_t *body) {
    switch (header->type) {
    case INKWELL_MQTT_CONNACK:
        return mqtt_on_connack(proxy, body, header->remaining);
    case INKWELL_MQTT_SUBACK:
        return mqtt_on_suback(proxy, body, header->remaining);
    case INKWELL_MQTT_PUBLISH:
        return mqtt_on_publish(proxy, header->flags, body, header->remaining);
    case INKWELL_MQTT_PINGRESP:
        /* Nothing to do: having arrived at all is the whole content, and `last_heard_ms` was
           already moved by the read that produced it. */
        return true;
    case INKWELL_MQTT_PUBACK:
    case INKWELL_MQTT_PUBREC:
    case INKWELL_MQTT_PUBREL:
    case INKWELL_MQTT_PUBCOMP:
    case INKWELL_MQTT_UNSUBACK:
        /* Answers to things this client never sends. Ignored rather than fatal - a broker that
           volunteers one is odd, not broken, and the stream is still in sync. */
        inkwell_log_debug("mqtt", "Ignoring an unexpected packet type %u", (unsigned)header->type);
        return true;
    case INKWELL_MQTT_CONNECT:
    case INKWELL_MQTT_SUBSCRIBE:
    case INKWELL_MQTT_UNSUBSCRIBE:
    case INKWELL_MQTT_PINGREQ:
    case INKWELL_MQTT_DISCONNECT:
    case INKWELL_MQTT_PACKET_NONE:
    default:
        /* Client-to-server packets arriving from the server. This is not a broker being
           eccentric, it is a stream that is being read at the wrong offset. */
        mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_PROTOCOL, 0U);
        return false;
    }
}

/*
 * Takes whole packets off the front of the inbound buffer.
 *
 * Returns false when the connection was dropped inside a handler, in which case the buffers have
 * already been reset and nothing here may touch them again.
 */
static bool mqtt_consume(struct mesh_mqtt_proxy *proxy) {
    size_t at = 0U;

    for (;;) {
        /*
         * An oversized message being counted off the stream. This is the case the whole
         * header/body split exists for: the bytes have to be consumed in order or every packet
         * after them is read at the wrong offset, but they must never be buffered, because a
         * broker may retain a message far larger than anything this client could forward.
         */
        if (proxy->skip_remaining > 0U) {
            size_t chunk = proxy->in_len - at;
            if (chunk > proxy->skip_remaining) {
                chunk = proxy->skip_remaining;
            }
            at += chunk;
            proxy->skip_remaining -= chunk;
            if (proxy->skip_remaining > 0U) {
                break; /* more of it still to come */
            }
            continue;
        }

        struct inkwell_mqtt_header header;
        const int decoded = inkwell_mqtt_decode_header(proxy->in + at, proxy->in_len - at, &header);
        if (decoded == 0) {
            break; /* not a whole header yet */
        }
        if (decoded < 0) {
            mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_PROTOCOL, 0U);
            return false;
        }

        if (header.remaining > sizeof proxy->in - header.header_len) {
            at += header.header_len;
            proxy->skip_remaining = header.remaining;
            proxy->stats.skipped++;
            inkwell_log_debug("mqtt", "Skipping a %zu-byte message", header.remaining);
            continue;
        }
        if (proxy->in_len - at < header.header_len + header.remaining) {
            break; /* the body is still arriving */
        }

        if (!mqtt_handle(proxy, &header, proxy->in + at + header.header_len)) {
            return false;
        }
        /*
         * A handler may have ended the connection while still reporting success - a CONNACK is
         * accepted and then the subscription behind it cannot be written. `mqtt_close()` has
         * zeroed `in_len` by then, so carrying on would subtract `at` from zero and walk this
         * loop off the buffer with a size_t the wrong side of nothing.
         */
        if (proxy->fd < 0) {
            return false;
        }
        at += header.header_len + header.remaining;
    }

    if (at > 0U) {
        proxy->in_len -= at;
        if (proxy->in_len > 0U) {
            memmove(proxy->in, proxy->in + at, proxy->in_len);
        }
    }
    return true;
}

/*
 * Reads what is ready.
 *
 * The buffer can never fill without a whole packet in it: anything whose body would not fit was
 * turned into a skip above, so whatever is being waited on is by construction smaller than
 * `in`. That is what makes it safe to stop reading when there is no room rather than having to
 * grow.
 */
static void mqtt_read_ready(struct mesh_mqtt_proxy *proxy) {
    proxy->more_to_read = false;
    proxy->read_wants_write = false;

    for (unsigned turn = 0U; turn < MQTT_READS_PER_TURN; ++turn) {
        if (proxy->in_len >= sizeof proxy->in) {
            proxy->more_to_read = true;
            return;
        }
        const int got =
            mqtt_raw_read(proxy, proxy->in + proxy->in_len, sizeof proxy->in - proxy->in_len);
        if (got == -EAGAIN) {
            /*
             * Not an empty socket: the session gave the loop back part-way through work of its
             * own, which today means a run of tickets long enough to hit its budget. Waiting for
             * a readiness event here would be waiting for one that may never come, so this takes
             * the same way back as a spent read budget does.
             */
            if (proxy->tls.state != NULL && proxy->tls.more_to_read) {
                proxy->more_to_read = true;
                return;
            }
            /* Which way it is blocked, remembered rather than assumed: a TLS read that stopped
               because it has something to send is woken by EPOLLOUT, and mqtt_fd_callback()
               reads the flag to know that a writable socket means *this* rather than the write
               queue. Without it the read is never retried, since nothing new arrives to raise
               EPOLLIN and epoll reports a writable socket forever. */
            proxy->read_wants_write = proxy->tls.state != NULL && proxy->tls.wants_write;
            mqtt_arm(proxy);
            return;
        }
        if (got == -ENOTCONN || got == -ECONNRESET) {
            mqtt_fail_net(proxy, INKWELL_NET_CLOSED, 0);
            return;
        }
        if (got < 0) {
            if (proxy->tls.state != NULL) {
                mqtt_fail_tls(proxy);
            } else {
                mqtt_fail_net(proxy, inkwell_net_reason_from_errno(got), got);
            }
            return;
        }

        proxy->in_len += (size_t)got;
        proxy->last_heard_ms = proxy->now_ms;
        if (!mqtt_consume(proxy)) {
            return;
        }
    }

    /*
     * The budget ran out with the socket still readable - and, under TLS, possibly with
     * plaintext already decrypted and sitting inside the session where epoll cannot see it. The
     * flag is what brings us back next turn; without it a TLS connection can wait forever on
     * bytes it has already received.
     */
    proxy->more_to_read = true;
}

/* ------------------------------------------------------------------ getting connected */

/* Sends the CONNECT and waits for the answer. Called once the transport - socket or TLS
   session - is ready to carry it. */
static void mqtt_send_connect(struct mesh_mqtt_proxy *proxy) {
    struct inkwell_mqtt_connect params;
    memset(&params, 0, sizeof params);
    params.client_id = proxy->config.client_id;
    params.username = proxy->config.username[0] != '\0' ? proxy->config.username : NULL;
    params.password = proxy->config.password[0] != '\0' ? proxy->config.password : NULL;
    params.keepalive_s = (uint16_t)MESH_MQTT_KEEPALIVE_S;
    /*
     * Always a clean session. The alternative asks the broker to hold undelivered messages for
     * us while we are away - which for a proxy is exactly wrong: what it would hold is a queue
     * of mesh traffic to replay at a radio whose users have long since walked out of range of
     * whatever it was about.
     */
    params.clean_session = true;

    uint8_t packet[MESH_MQTT_CLIENT_ID_MAX + MESH_MQTT_USERNAME_MAX + MESH_MQTT_PASSWORD_MAX + 32U];
    const int len = inkwell_mqtt_encode_connect(packet, sizeof packet, &params);
    if (len < 0) {
        mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_BAD_ADDRESS, 0U);
        return;
    }
    if (mqtt_queue(proxy, packet, (size_t)len) < 0) {
        mqtt_fail_net(proxy, inkwell_net_reason_from_errno(ENOBUFS), -ENOBUFS);
        return;
    }
    proxy->deadline_ms = proxy->now_ms + MESH_MQTT_HANDSHAKE_TIMEOUT_MS;
    proxy->last_heard_ms = proxy->now_ms;
    mqtt_set_state(proxy, MESH_MQTT_PROXY_GREETING);
}

/* Drives the TLS handshake to completion, then hands over to the MQTT one. */
static void mqtt_secure(struct mesh_mqtt_proxy *proxy) {
    const int rc = mesh_tls_client_handshake(&proxy->tls);
    if (rc == -EAGAIN) {
        mqtt_arm(proxy);
        return;
    }
    if (rc < 0) {
        mqtt_fail_tls(proxy);
        return;
    }
    mqtt_send_connect(proxy);
}

/* The TCP connect finished, one way or the other. */
static void mqtt_finish_connect(struct mesh_mqtt_proxy *proxy) {
    int error = 0;
    socklen_t error_len = (socklen_t)sizeof error;
    if (getsockopt(proxy->fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0) {
        error = errno;
    }
    if (error != 0) {
        mqtt_fail_net(proxy, inkwell_net_reason_from_errno(error), -error);
        return;
    }

    if (!proxy->config.tls_enabled) {
        mqtt_send_connect(proxy);
        return;
    }

    /*
     * The certificate is checked against the *name*, not the address it resolved to. A
     * certificate is issued for a name, and no broker on the internet has one for an IP - so
     * verifying against the resolved address would fail every TLS connection this client makes.
     */
    const int started =
        mesh_tls_client_start(&proxy->tls, proxy->fd, proxy->host, proxy->ca_bundle);
    if (started == -ENOTSUP) {
        mqtt_fail_own(proxy, MESH_MQTT_REFUSAL_NO_TLS, 0U);
        return;
    }
    if (started < 0) {
        mqtt_fail_tls(proxy);
        return;
    }
    proxy->deadline_ms = proxy->now_ms + MESH_MQTT_HANDSHAKE_TIMEOUT_MS;
    mqtt_set_state(proxy, MESH_MQTT_PROXY_SECURING);
    mqtt_secure(proxy);
}

static int mqtt_fd_callback(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct mesh_mqtt_proxy *proxy = (struct mesh_mqtt_proxy *)userdata;
    if (proxy == NULL || proxy->fd < 0) {
        return 0;
    }

    if (proxy->state == MESH_MQTT_PROXY_CONNECTING) {
        /* EPOLLERR here is the ordinary refusal - nothing listening on that port - and
           getsockopt() is what turns it into the errno that says so. */
        if ((events & (uint32_t)(EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0U) {
            mqtt_finish_connect(proxy);
        }
        return 0;
    }

    if ((events & (uint32_t)(EPOLLERR | EPOLLHUP)) != 0U) {
        mqtt_fail_net(proxy, INKWELL_NET_CLOSED, 0);
        return 0;
    }

    if (proxy->state == MESH_MQTT_PROXY_SECURING) {
        /* One call covers both directions: the handshake itself decides which way it is blocked
           and mqtt_arm() follows it. */
        mqtt_secure(proxy);
        return 0;
    }

    bool read_now = (events & (uint32_t)EPOLLIN) != 0U;

    if ((events & (uint32_t)EPOLLOUT) != 0U) {
        /*
         * A writable socket is two different pieces of news, and the flag says which. Usually it
         * is the write queue's turn; but a TLS read that stopped mid-record because it has an
         * alert or a handshake message to send is also waiting on exactly this event, and
         * flushing an empty queue would answer neither. Read before the flush decides anything,
         * because a successful write clears `tls.wants_write` underneath it.
         */
        if (proxy->read_wants_write) {
            read_now = true;
        }
        const int flushed = mqtt_flush(proxy);
        if (flushed < 0) {
            mqtt_fail_net(proxy, INKWELL_NET_CLOSED, 0);
            return 0;
        }
    }
    if (read_now && proxy->fd >= 0) {
        mqtt_read_ready(proxy);
    }
    return 0;
}

/* Opens the socket and starts the connect. `address` already carries the port. */
static void mqtt_open(struct mesh_mqtt_proxy *proxy, const struct sockaddr_storage *address,
                      socklen_t address_len) {
    const int fd = socket(address->ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        mqtt_fail_net(proxy, inkwell_net_reason_from_errno(errno), -errno);
        return;
    }
    /* MQTT packets are small and a reply often follows a request immediately, which is exactly
       the traffic Nagle delays. */
    const int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    if (connect(fd, (const struct sockaddr *)address, address_len) < 0 && errno != EINPROGRESS) {
        const int error = errno;
        close(fd);
        mqtt_fail_net(proxy, inkwell_net_reason_from_errno(error), -error);
        return;
    }

    proxy->fd = fd;
    proxy->want_write = true;
    if (inkwell_loop_add_fd(proxy->loop, fd, (uint32_t)(EPOLLIN | EPOLLOUT), mqtt_fd_callback,
                            proxy) < 0) {
        proxy->fd = -1;
        close(fd);
        mqtt_fail_net(proxy, inkwell_net_reason_from_errno(ENOMEM), -ENOMEM);
        return;
    }
    proxy->fd_registered = true;
    proxy->deadline_ms = proxy->now_ms + MESH_MQTT_CONNECT_TIMEOUT_MS;
    mqtt_set_state(proxy, MESH_MQTT_PROXY_CONNECTING);
}

static void mqtt_on_resolved(void *userdata, const struct inkwell_resolve_result *result) {
    struct mesh_mqtt_proxy *proxy = (struct mesh_mqtt_proxy *)userdata;
    /* A lookup that lands after the attempt it belonged to was abandoned. Cancelling normally
       prevents this; the guard is what makes it true rather than nearly true. */
    if (proxy->state != MESH_MQTT_PROXY_RESOLVING) {
        return;
    }
    if (result->outcome == INKWELL_RESOLVE_OK) {
        mqtt_open(proxy, &result->address, result->address_len);
        return;
    }
    /*
     * Three outcomes into three reasons, one for one. The switch this replaces folded the
     * timeout in with the failure because both said the same sentence; inkwell keeps them apart
     * and what to say about them is now somebody else's decision, so there is nothing left here
     * to collapse. `error` is the EAI_* the child got - worth logging, not worth showing.
     */
    mqtt_fail_net(proxy, inkwell_net_reason_from_resolve(result->outcome), result->error);
}

/*
 * One connection attempt, from whatever the address turns out to be.
 *
 * Both entry points into this - a fresh start() and the backoff expiring - go through here, so
 * there is exactly one description of what an attempt is.
 */
static void mqtt_attempt(struct mesh_mqtt_proxy *proxy) {
    memset(&proxy->failure, 0, sizeof proxy->failure);

    struct sockaddr_storage address;
    socklen_t address_len = 0;
    if (inkwell_resolve_literal(proxy->host, proxy->port, &address, &address_len)) {
        mqtt_open(proxy, &address, address_len);
        return;
    }

    const int started = inkwell_resolve_start(&proxy->resolve, proxy->host, proxy->port,
                                              mqtt_on_resolved, proxy, proxy->now_ms);
    if (started < 0) {
        mqtt_fail_net(proxy, INKWELL_NET_LOOKUP_FAILED, started);
        return;
    }
    proxy->deadline_ms = 0U; /* the resolver enforces its own */
    mqtt_set_state(proxy, MESH_MQTT_PROXY_RESOLVING);
}

/* ------------------------------------------------------------------ teardown */

/*
 * Releases the connection without deciding what happens next. Every caller is either failing it
 * (mqtt_fail) or stopping it (mesh_mqtt_proxy_stop), and the state it leaves behind is theirs
 * to set - which is why this one does not set a state at all.
 */
static void mqtt_close(struct mesh_mqtt_proxy *proxy) {
    inkwell_resolve_cancel(&proxy->resolve);
    mesh_tls_client_stop(&proxy->tls);
    if (proxy->fd >= 0) {
        if (proxy->fd_registered && proxy->loop != NULL) {
            (void)inkwell_loop_remove_fd(proxy->loop, proxy->fd);
        }
        close(proxy->fd);
    }
    proxy->fd = -1;
    proxy->fd_registered = false;
    proxy->want_write = false;
    proxy->in_len = 0U;
    proxy->skip_remaining = 0U;
    proxy->more_to_read = false;
    proxy->read_wants_write = false;
    proxy->out_len = 0U;
    proxy->out_sent = 0U;
    proxy->out_pending = 0U;
    proxy->filter_sent = 0U;
    proxy->subscribe_id = 0U;
    proxy->deadline_ms = 0U;
    proxy->next_ping_ms = 0U;
    proxy->last_heard_ms = 0U;
}

/* ------------------------------------------------------------------ the public face */

int mesh_mqtt_proxy_init(struct mesh_mqtt_proxy *proxy, struct inkwell_loop *loop) {
    if (proxy == NULL) {
        return -EINVAL;
    }
    memset(proxy, 0, sizeof *proxy);
    proxy->loop = loop;
    proxy->fd = -1;
    proxy->state = MESH_MQTT_PROXY_OFF;
    (void)inkwell_resolve_init(&proxy->resolve, loop);
    return 0;
}

void mesh_mqtt_proxy_shutdown(struct mesh_mqtt_proxy *proxy) {
    if (proxy == NULL) {
        return;
    }
    mqtt_close(proxy);
    inkwell_resolve_shutdown(&proxy->resolve);
    proxy->state = MESH_MQTT_PROXY_OFF;
}

void mesh_mqtt_proxy_set_ca_bundle(struct mesh_mqtt_proxy *proxy, const char *path) {
    if (proxy == NULL) {
        return;
    }
    if (path == NULL) {
        proxy->ca_bundle[0] = '\0';
        return;
    }
    (void)inkwell_str_copy(proxy->ca_bundle, sizeof proxy->ca_bundle, path);
}

int mesh_mqtt_proxy_start(struct mesh_mqtt_proxy *proxy,
                          const struct mesh_mqtt_proxy_config *config,
                          mesh_mqtt_proxy_message_fn on_message, mesh_mqtt_proxy_state_fn on_state,
                          void *userdata, uint64_t now_ms) {
    if (proxy == NULL || config == NULL) {
        return -EINVAL;
    }
    if (proxy->loop == NULL) {
        return -ENOTSUP;
    }
    if (config->address[0] == '\0' || config->client_id[0] == '\0') {
        return -EINVAL;
    }

    /*
     * The address is split before anything is opened, so a target that cannot be parsed is
     * refused rather than becoming a connection attempt that fails for a reason nobody can read.
     * mesh_tcp_target_split() is reused deliberately: "host", "host:port" and "[v6]:port" mean
     * exactly the same thing to a broker as to a radio, and a second parser would be a second
     * set of bracket rules to get subtly different.
     */
    char host[MESH_MQTT_ADDRESS_MAX];
    uint16_t port = 0U;
    if (mesh_tcp_target_split(config->address, host, sizeof host, &port) != 0) {
        return -EINVAL;
    }

    mqtt_close(proxy);
    proxy->now_ms = now_ms;
    proxy->config = *config;
    proxy->on_message = on_message;
    proxy->on_state = on_state;
    proxy->userdata = userdata;
    proxy->failures = 0U;
    proxy->retry_at_ms = 0U;
    memset(&proxy->failure, 0, sizeof proxy->failure);
    (void)inkwell_str_copy(proxy->host, sizeof proxy->host, host);

    /*
     * A target that named no port gets the one that goes with the scheme rather than a single
     * default, because the wrong one of the two does not fail quickly: 1883 against a TLS
     * listener is a handshake that hangs, and 8883 against a plaintext one is a broker that
     * reads our CONNECT as a ClientHello.
     *
     * mesh_tcp_target_split() has no way to say "no port was given", so this compares against
     * the default it substitutes. The cost is that an explicit `:4403` on an MQTT address is
     * treated as unset - a port nobody would ever type at a broker.
     */
    if (port == (uint16_t)MESH_TCP_DEFAULT_PORT) {
        port = config->tls_enabled ? (uint16_t)MESH_MQTT_PORT_TLS : (uint16_t)MESH_MQTT_PORT;
    }
    proxy->port = port;

    mqtt_attempt(proxy);
    return 0;
}

void mesh_mqtt_proxy_stop(struct mesh_mqtt_proxy *proxy) {
    if (proxy == NULL || proxy->state == MESH_MQTT_PROXY_OFF) {
        return;
    }
    /*
     * A DISCONNECT on the way out, best effort and never waited for. Without one the broker sees
     * a client that stopped answering, which is not the same thing at all: it holds the session
     * open until the keepalive expires, and any will message it was told about would fire.
     */
    if (proxy->state == MESH_MQTT_PROXY_READY) {
        uint8_t packet[2];
        const int len = inkwell_mqtt_encode_empty(packet, sizeof packet, INKWELL_MQTT_DISCONNECT);
        if (len > 0) {
            (void)mqtt_raw_write(proxy, packet, (size_t)len);
        }
    }
    mqtt_close(proxy);
    memset(&proxy->failure, 0, sizeof proxy->failure);
    proxy->failures = 0U;
    proxy->retry_at_ms = 0U;
    mqtt_set_state(proxy, MESH_MQTT_PROXY_OFF);
}

int mesh_mqtt_proxy_subscribe(struct mesh_mqtt_proxy *proxy, const char *filter) {
    if (proxy == NULL || filter == NULL) {
        return -EINVAL;
    }
    const size_t len = strlen(filter);
    if (len == 0U || len >= MESH_MQTT_FILTER_MAX) {
        return -EINVAL;
    }
    for (size_t i = 0U; i < proxy->filter_count; ++i) {
        if (strcmp(proxy->filters[i], filter) == 0) {
            return -EEXIST;
        }
    }
    if (proxy->filter_count >= MESH_MQTT_FILTERS_MAX) {
        return -ENOSPC;
    }
    memcpy(proxy->filters[proxy->filter_count], filter, len + 1U);
    proxy->filter_count++;
    mqtt_pump_subscribes(proxy);
    return 0;
}

void mesh_mqtt_proxy_clear_filters(struct mesh_mqtt_proxy *proxy) {
    if (proxy == NULL) {
        return;
    }
    proxy->filter_count = 0U;
    proxy->filter_sent = 0U;
    proxy->subscribe_id = 0U;
}

int mesh_mqtt_proxy_publish(struct mesh_mqtt_proxy *proxy, const char *topic,
                            const uint8_t *payload, size_t len, bool retained) {
    if (proxy == NULL || topic == NULL) {
        return -EINVAL;
    }
    if (proxy->state != MESH_MQTT_PROXY_READY) {
        proxy->stats.dropped++;
        return -ENOTCONN;
    }
    /* A wildcard is legal in a filter and forbidden in a published topic, and a broker's answer
       to one is to drop the connection rather than to say so - which would present as a link
       that flaps whenever one particular channel has traffic. */
    if (!inkwell_mqtt_topic_is_publishable(topic)) {
        proxy->stats.dropped++;
        return -EINVAL;
    }

    uint8_t packet[MESH_MQTT_PACKET_MAX];
    const int encoded =
        inkwell_mqtt_encode_publish(packet, sizeof packet, topic, payload, len, retained);
    if (encoded < 0) {
        proxy->stats.dropped++;
        return encoded == -ENOSPC ? -EMSGSIZE : encoded;
    }
    const int queued = mqtt_queue(proxy, packet, (size_t)encoded);
    if (queued < 0) {
        proxy->stats.dropped++;
        /* A failed *write* is fatal to the connection; a full buffer is not. The first says the
           socket is gone, the second says it has not drained yet. */
        if (queued != -ENOSPC) {
            mqtt_fail_net(proxy, INKWELL_NET_CLOSED, 0);
        }
        return queued;
    }
    proxy->stats.published++;
    return 0;
}

void mesh_mqtt_proxy_tick(struct mesh_mqtt_proxy *proxy, uint64_t now_ms) {
    if (proxy == NULL) {
        return;
    }
    /*
     * The clock, read once and kept.
     *
     * Everything inside this module - a deadline, the keepalive, the backoff - reads
     * `proxy->now_ms` rather than calling inkwell_time_monotonic_ms() where it happens to be. That
     * matters because half of those are *set* from an fd callback and compared here: two calls
     * to the real clock are two different numbers, and a test driving a synthetic one would have
     * been comparing its own clock against the machine's. One source, set here, slightly stale
     * inside a callback and monotonic either way.
     */
    proxy->now_ms = now_ms;
    inkwell_resolve_tick(&proxy->resolve, now_ms);

    if (proxy->state == MESH_MQTT_PROXY_OFF) {
        return;
    }
    if (proxy->state == MESH_MQTT_PROXY_WAITING) {
        if (now_ms >= proxy->retry_at_ms) {
            mqtt_attempt(proxy);
        }
        return;
    }

    /* A deadline on every state that is waiting for the far end to do something, so none of them
       can park on a socket that will never answer. */
    if (proxy->deadline_ms != 0U && now_ms >= proxy->deadline_ms) {
        mqtt_fail_net(proxy, INKWELL_NET_TIMED_OUT, 0);
        return;
    }

    /*
     * Data already decrypted inside the TLS session, or a read budget that ran out. epoll has
     * nothing left to report in the first case, so this is the only thing that comes back.
     *
     * GREETING as well as READY, because the CONNACK is read by that same function: a session
     * that stopped early on its way to one - on a run of tickets, say - has the answer sitting
     * inside it with an empty socket underneath, and gating this on READY alone would leave it
     * there until the deadline and then retry into the same place forever.
     */
    if (proxy->more_to_read &&
        (proxy->state == MESH_MQTT_PROXY_GREETING || proxy->state == MESH_MQTT_PROXY_READY)) {
        mqtt_read_ready(proxy);
    }

    if (proxy->state != MESH_MQTT_PROXY_READY) {
        return;
    }

    /* The broker has said nothing for long enough that the connection is not there any more.
       This is what notices a network that went away without anybody closing anything - a Brick
       carried out of WiFi range, most often. */
    if (proxy->last_heard_ms != 0U && now_ms > proxy->last_heard_ms &&
        now_ms - proxy->last_heard_ms > MESH_MQTT_SILENCE_TIMEOUT_MS) {
        mqtt_fail_net(proxy, INKWELL_NET_TIMED_OUT, 0);
        return;
    }

    if (proxy->next_ping_ms != 0U && now_ms >= proxy->next_ping_ms) {
        uint8_t packet[2];
        const int len = inkwell_mqtt_encode_empty(packet, sizeof packet, INKWELL_MQTT_PINGREQ);
        if (len > 0 && mqtt_queue(proxy, packet, (size_t)len) < 0) {
            mqtt_fail_net(proxy, INKWELL_NET_CLOSED, 0);
        }
    }
}

enum mesh_mqtt_proxy_state mesh_mqtt_proxy_state(const struct mesh_mqtt_proxy *proxy) {
    return proxy != NULL ? proxy->state : MESH_MQTT_PROXY_OFF;
}

bool mesh_mqtt_proxy_is_ready(const struct mesh_mqtt_proxy *proxy) {
    return proxy != NULL && proxy->state == MESH_MQTT_PROXY_READY;
}

struct mesh_mqtt_proxy_stats mesh_mqtt_proxy_stats(const struct mesh_mqtt_proxy *proxy) {
    struct mesh_mqtt_proxy_stats empty;
    memset(&empty, 0, sizeof empty);
    return proxy != NULL ? proxy->stats : empty;
}

const char *mesh_mqtt_failure_name(const struct mesh_mqtt_proxy_failure *failure) {
    if (failure == NULL) {
        return "none";
    }
    /* The refusal half first: it is the one that is set when `net.reason` is still
       INKWELL_NET_OK, and "ok" is not something a failed attempt should ever log. */
    if (failure->refusal != MESH_MQTT_REFUSAL_NONE) {
        return mqtt_refusal_name(failure->refusal);
    }
    if (failure->net.reason == INKWELL_NET_OK) {
        return "none";
    }
    return inkwell_net_reason_name(failure->net.reason);
}

struct mesh_mqtt_proxy_failure mesh_mqtt_proxy_failure(const struct mesh_mqtt_proxy *proxy) {
    struct mesh_mqtt_proxy_failure none;
    memset(&none, 0, sizeof none);
    return proxy != NULL ? proxy->failure : none;
}

const char *mesh_mqtt_proxy_tls_error(const struct mesh_mqtt_proxy *proxy) {
    return proxy != NULL ? mesh_tls_client_error(&proxy->tls) : "";
}

const char *mesh_mqtt_proxy_address(const struct mesh_mqtt_proxy *proxy) {
    return proxy != NULL ? proxy->config.address : "";
}

const char *mesh_mqtt_proxy_host(const struct mesh_mqtt_proxy *proxy) {
    return proxy != NULL ? proxy->host : "";
}
