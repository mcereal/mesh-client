#define _POSIX_C_SOURCE 200809L

/*
 * The MQTT client proxy, against a broker.
 *
 * Not a scripted fake: every case here stands a real listening socket on loopback, lets the
 * proxy connect to it through the real event loop, and speaks MQTT back at it by hand. That is
 * more setup than a table of injected buffers and it buys the things a fake cannot - a real
 * non-blocking connect, real partial reads, a real EOF when the broker hangs up, and a stream
 * whose framing has to stay in sync across all of it.
 *
 * The clock is synthetic throughout. `mesh_mqtt_proxy_tick()` takes the time rather than reading
 * it, so a case can step past a five-second backoff or a ninety-second silence timeout without
 * waiting for either - which is the only reason those paths are testable at all.
 */

#include "framework/mesh_test.h"

#include "mesh/core/event_loop.h"
#include "mesh/core/mqtt_proxy.h"
#include "mesh/proto/mqtt_packet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------------ the broker */

struct broker_tls;

struct fake_broker {
    int listener;
    int client; /* the accepted connection, or -1 */
    uint16_t port;
    /* Set before the proxy connects: the accepted socket is wrapped in a TLS session rather
       than read directly. */
    bool tls;
    struct broker_tls *sec;
    uint8_t in[8192];
    size_t in_len;
};

/* ------------------------------------------------------------------ TLS */

#ifdef MESHCLIENT_HAVE_TLS

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <stdlib.h>

/*
 * A TLS broker, so the client's TLS path completes a real handshake under test.
 *
 * This is the reason `MBEDTLS_SSL_SRV_C` is left enabled in
 * third_party/mbedtls-config/mesh_mbedtls_config.h. The alternative was to check the TLS path
 * only as far as "it tried", which for a security boundary is not a check at all: a client that
 * never verifies a certificate and a client that always does look identical until something
 * presents a bad one. Here, one case connects to a broker whose certificate is in the bundle and
 * one to a broker whose certificate is not, and they have to come out differently.
 *
 * The certificate and key below were generated for this file, are valid from 2020 to 2120, and
 * exist nowhere else. They are not a secret in any sense - the key is published here precisely
 * so the broker can be stood up without one.
 */
/* The broker's certificate, also used as the client's whole CA bundle. */
static const char broker_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBrjCCAVSgAwIBAgIUJBfYqvuGth5Y/EVGDDUiDILgEtkwCgYIKoZIzj0EAwIw\n"
    "FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTIwMDEwMTAwMDAwMFoYDzIxMjAwMTAx\n"
    "MDAwMDAwWjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO\n"
    "PQMBBwNCAAST7A+SQZZOWG6kiqu6iCE6yW/sSUIXRYrt61KUWC7Hl2XLXd4CHVm+\n"
    "URT+1L2hHbpU8YuNHjhaJCIUr6gE9tSFo4GBMH8wHQYDVR0OBBYEFOehhqGUIxfI\n"
    "++8HRAYb+guI2IbdMB8GA1UdIwQYMBaAFOehhqGUIxfI++8HRAYb+guI2IbdMCwG\n"
    "A1UdEQQlMCOCCWxvY2FsaG9zdIcEfwAAAYcQAAAAAAAAAAAAAAAAAAAAATAPBgNV\n"
    "HRMBAf8EBTADAQH/MAoGCCqGSM49BAMCA0gAMEUCIBhB1njQfIB+cFODfX6QD0tK\n"
    "38VOSfermV5PSnKSrvbVAiEAkP61Go8AGcuZHxtwK5tkPza1qucObtlkfuSuOtm7\n"
    "yO0=\n"
    "-----END CERTIFICATE-----\n";

/* Its private key. Generated for this file and used nowhere else. */
static const char broker_key_pem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgqFZM7GZZgzVkEGbX\n"
    "7FzKs1i6yw5YW5iIBbZoXBq29FShRANCAAST7A+SQZZOWG6kiqu6iCE6yW/sSUIX\n"
    "RYrt61KUWC7Hl2XLXd4CHVm+URT+1L2hHbpU8YuNHjhaJCIUr6gE9tSF\n"
    "-----END PRIVATE KEY-----\n";

/* An unrelated self-signed certificate, for the case where the bundle does not
   contain the broker's. */
static const char stranger_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBizCCATGgAwIBAgIUdOS6V6z6I+dQJwx+vDgY2XZzTjAwCgYIKoZIzj0EAwIw\n"
    "GjEYMBYGA1UEAwwPbm90LXRoaXMtYnJva2VyMCAXDTIwMDEwMTAwMDAwMFoYDzIx\n"
    "MjAwMTAxMDAwMDAwWjAaMRgwFgYDVQQDDA9ub3QtdGhpcy1icm9rZXIwWTATBgcq\n"
    "hkjOPQIBBggqhkjOPQMBBwNCAATHX9uXwjXOckZ3EMx5W9bXFGlRYXsenrikRQ6j\n"
    "FM0W2QdkAPPC0CgEvHKNkVY2F5a1E7QdiAwwtwI55/s1zi6po1MwUTAdBgNVHQ4E\n"
    "FgQUJJN6KD+/8thQT8CboU409Sghuh4wHwYDVR0jBBgwFoAUJJN6KD+/8thQT8Cb\n"
    "oU409Sghuh4wDwYDVR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNIADBFAiBQau0r\n"
    "ASVUcGJAmpKQPtbyLuQFLRsEi4RQLd8AYRVc1gIhAI1N7KvTvVqmdIj0gK6VJKnR\n"
    "zgZLblKuSfqMw2EVRJnG\n"
    "-----END CERTIFICATE-----\n";

struct broker_tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool handshaked;
};

/* The same non-blocking BIO the client uses, for the same reason: both ends of this handshake
   are driven from one thread, so a blocking read on either would be a deadlock rather than a
   wait. */
static int broker_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    const int fd = (int)(intptr_t)ctx;
    const ssize_t written = send(fd, buf, len, MSG_NOSIGNAL);
    if (written >= 0) {
        return (int)written;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE
                                                     : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int broker_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    const int fd = (int)(intptr_t)ctx;
    const ssize_t got = recv(fd, buf, len, 0);
    if (got > 0) {
        return (int)got;
    }
    if (got == 0) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ
                                                     : MBEDTLS_ERR_NET_RECV_FAILED;
}

static bool broker_tls_start(struct fake_broker *broker) {
    struct broker_tls *sec = calloc(1U, sizeof *sec);
    if (sec == NULL) {
        return false;
    }
    broker->sec = sec;
    mbedtls_ssl_init(&sec->ssl);
    mbedtls_ssl_config_init(&sec->conf);
    mbedtls_x509_crt_init(&sec->cert);
    mbedtls_pk_init(&sec->key);
    mbedtls_entropy_init(&sec->entropy);
    mbedtls_ctr_drbg_init(&sec->drbg);

    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    static const unsigned char seed[] = "meshclient-test-broker";
    if (mbedtls_ctr_drbg_seed(&sec->drbg, mbedtls_entropy_func, &sec->entropy, seed,
                              sizeof seed - 1U) != 0) {
        return false;
    }
    /* The lengths include the terminator: mbedtls_x509_crt_parse() requires it for PEM. */
    if (mbedtls_x509_crt_parse(&sec->cert, (const unsigned char *)broker_cert_pem,
                               sizeof broker_cert_pem) != 0) {
        return false;
    }
    if (mbedtls_pk_parse_key(&sec->key, (const unsigned char *)broker_key_pem,
                             sizeof broker_key_pem, NULL, 0U, mbedtls_ctr_drbg_random,
                             &sec->drbg) != 0) {
        return false;
    }
    if (mbedtls_ssl_config_defaults(&sec->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return false;
    }
    mbedtls_ssl_conf_rng(&sec->conf, mbedtls_ctr_drbg_random, &sec->drbg);
    /* No client certificate is asked for: what is under test is the client checking the
     *server*, which is the direction a broker connection actually depends on. */
    mbedtls_ssl_conf_authmode(&sec->conf, MBEDTLS_SSL_VERIFY_NONE);
    if (mbedtls_ssl_conf_own_cert(&sec->conf, &sec->cert, &sec->key) != 0) {
        return false;
    }
    if (mbedtls_ssl_setup(&sec->ssl, &sec->conf) != 0) {
        return false;
    }
    mbedtls_ssl_set_bio(&sec->ssl, (void *)(intptr_t)broker->client, broker_bio_send,
                        broker_bio_recv, NULL);
    return true;
}

static void broker_tls_stop(struct fake_broker *broker) {
    struct broker_tls *sec = broker->sec;
    if (sec == NULL) {
        return;
    }
    mbedtls_ssl_free(&sec->ssl);
    mbedtls_ssl_config_free(&sec->conf);
    mbedtls_x509_crt_free(&sec->cert);
    mbedtls_pk_free(&sec->key);
    mbedtls_ctr_drbg_free(&sec->drbg);
    mbedtls_entropy_free(&sec->entropy);
    free(sec);
    broker->sec = NULL;
}

/* Writes a PEM out where mbedtls_x509_crt_parse_file() can find it: the client takes its CA
   bundle as a path, because on the device it is a file the pak ships. */
static bool write_pem(const char *path, const char *pem) {
    FILE *out = fopen(path, "w");
    if (out == NULL) {
        return false;
    }
    const bool ok = fputs(pem, out) >= 0;
    return fclose(out) == 0 && ok;
}

#else /* !MESHCLIENT_HAVE_TLS */

static bool broker_tls_start(struct fake_broker *broker) {
    (void)broker;
    return false;
}

static void broker_tls_stop(struct fake_broker *broker) { (void)broker; }

#endif /* MESHCLIENT_HAVE_TLS */

static void broker_init(struct fake_broker *broker) {
    memset(broker, 0, sizeof *broker);
    broker->listener = -1;
    broker->client = -1;
}

static bool broker_listen(struct fake_broker *broker) {
    broker->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (broker->listener < 0) {
        return false;
    }
    const int one = 1;
    (void)setsockopt(broker->listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0; /* the kernel picks, so two cases never collide */
    if (bind(broker->listener, (const struct sockaddr *)&address, sizeof address) < 0 ||
        listen(broker->listener, 1) < 0) {
        return false;
    }
    socklen_t len = (socklen_t)sizeof address;
    if (getsockname(broker->listener, (struct sockaddr *)&address, &len) < 0) {
        return false;
    }
    broker->port = ntohs(address.sin_port);
    return true;
}

static void broker_close(struct fake_broker *broker) {
    broker_tls_stop(broker);
    if (broker->client >= 0) {
        close(broker->client);
        broker->client = -1;
    }
    if (broker->listener >= 0) {
        close(broker->listener);
        broker->listener = -1;
    }
}

/* Takes the connection if one is waiting. Not an error when none is: the proxy's connect and
   this accept are driven from the same thread, so which lands first is a matter of timing. */
static bool broker_accept(struct fake_broker *broker) {
    if (broker->client >= 0) {
        return true;
    }
    struct pollfd waiting = {.fd = broker->listener, .events = POLLIN, .revents = 0};
    if (poll(&waiting, 1, 0) <= 0) {
        return false;
    }
    broker->client = accept(broker->listener, NULL, NULL);
    if (broker->client < 0) {
        return false;
    }
    /*
     * Non-blocking, which is not a detail. The proxy's side of every exchange here only advances
     * when the event loop runs, and the event loop only runs between calls into this fixture -
     * so a blocking read on the broker's socket is not a wait, it is a deadlock.
     */
    (void)fcntl(broker->client, F_SETFL, fcntl(broker->client, F_GETFL, 0) | O_NONBLOCK);
    if (broker->tls && !broker_tls_start(broker)) {
        close(broker->client);
        broker->client = -1;
        return false;
    }
    return true;
}

/* One read, however this connection reads. Returns the byte count, or <= 0 for "nothing now". */
static ssize_t broker_recv(struct fake_broker *broker, uint8_t *out, size_t cap) {
#ifdef MESHCLIENT_HAVE_TLS
    if (broker->sec != NULL) {
        /* The handshake first, driven a step at a time from the same turns that drive the
           client's. Neither side can finish without the other being run. */
        if (!broker->sec->handshaked) {
            if (mbedtls_ssl_handshake(&broker->sec->ssl) != 0) {
                return 0;
            }
            broker->sec->handshaked = true;
        }
        const int got = mbedtls_ssl_read(&broker->sec->ssl, out, cap);
        return got > 0 ? (ssize_t)got : 0;
    }
#endif
    return recv(broker->client, out, cap, MSG_DONTWAIT);
}

static void broker_pump(struct fake_broker *broker) {
    if (broker->client < 0) {
        return;
    }
    for (;;) {
        if (broker->in_len >= sizeof broker->in) {
            return;
        }
        const ssize_t got =
            broker_recv(broker, broker->in + broker->in_len, sizeof broker->in - broker->in_len);
        if (got <= 0) {
            return;
        }
        broker->in_len += (size_t)got;
    }
}

/*
 * Takes one whole MQTT packet off what the broker has received, or reports that there is not one
 * yet. `body` points into the broker's own buffer and is valid until the next call.
 */
static bool broker_take(struct fake_broker *broker, struct mesh_mqtt_header *header,
                        const uint8_t **body) {
    broker_pump(broker);
    if (mesh_mqtt_decode_header(broker->in, broker->in_len, header) <= 0) {
        return false;
    }
    const size_t whole = header->header_len + header->remaining;
    if (broker->in_len < whole) {
        return false;
    }
    /* Copied off the front rather than indexed, so the caller's pointer stays valid while the
       rest of the stream shuffles down behind it. */
    static uint8_t held[4096];
    memcpy(held, broker->in + header->header_len, header->remaining);
    memmove(broker->in, broker->in + whole, broker->in_len - whole);
    broker->in_len -= whole;
    *body = held;
    return true;
}

/*
 * Whether `needle` appears in `haystack`.
 *
 * Written out rather than reaching for the GNU extension that does this. That one needs
 * _GNU_SOURCE, which this suite has no other reason to ask for - and without it the call is
 * implicitly declared, returns `int`, and quietly truncates the pointer being compared against
 * NULL. Which is to say: the shortcut compiles, passes, and means nothing.
 */
static bool holds(const uint8_t *haystack, size_t len, const char *needle) {
    const size_t needle_len = strlen(needle);
    if (needle_len == 0U || len < needle_len) {
        return false;
    }
    for (size_t at = 0U; at + needle_len <= len; ++at) {
        if (memcmp(haystack + at, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static void broker_send(struct fake_broker *broker, const uint8_t *data, size_t len) {
    if (broker->client < 0) {
        return;
    }
#ifdef MESHCLIENT_HAVE_TLS
    if (broker->sec != NULL) {
        /* Only ever called after a case has seen a packet arrive, which means the handshake
           finished - so there is no partial-write case to carry here. */
        (void)mbedtls_ssl_write(&broker->sec->ssl, data, len);
        return;
    }
#endif
    (void)send(broker->client, data, len, MSG_NOSIGNAL);
}

static void broker_connack(struct fake_broker *broker, uint8_t code) {
    const uint8_t packet[] = {0x20U, 0x02U, 0x00U, code};
    broker_send(broker, packet, sizeof packet);
}

static void broker_suback(struct fake_broker *broker, uint16_t id, uint8_t code) {
    const uint8_t packet[] = {0x90U, 0x03U, (uint8_t)(id >> 8), (uint8_t)(id & 0xFFU), code};
    broker_send(broker, packet, sizeof packet);
}

/* ------------------------------------------------------------------ the harness */

struct proxy_probe {
    struct mesh_event_loop loop;
    struct mesh_mqtt_proxy proxy;
    struct fake_broker broker;
    uint64_t now_ms;

    unsigned messages;
    char last_topic[MESH_MQTT_TOPIC_MAX];
    uint8_t last_payload[512];
    size_t last_payload_len;
};

static void probe_on_message(void *userdata, const char *topic, const uint8_t *payload,
                             size_t len) {
    struct proxy_probe *probe = (struct proxy_probe *)userdata;
    probe->messages++;
    snprintf(probe->last_topic, sizeof probe->last_topic, "%s", topic);
    probe->last_payload_len = len < sizeof probe->last_payload ? len : sizeof probe->last_payload;
    if (probe->last_payload_len > 0U) {
        memcpy(probe->last_payload, payload, probe->last_payload_len);
    }
}

/*
 * One turn of everything: the broker takes a connection if one is offered, the event loop
 * delivers whatever is ready, and the proxy's clock advances by a step.
 *
 * The step is small so a case that wants to cross a real deadline says so by taking many turns
 * or by setting `probe->now_ms` directly, rather than by having the number of turns silently
 * decide how much time passed.
 */
static void probe_turn(struct proxy_probe *probe, uint64_t step_ms) {
    (void)broker_accept(&probe->broker);
    /*
     * The broker runs every turn, not only when a case asks it for a packet. A real one is
     * always reading, and under TLS it has to be: its half of the handshake only advances when
     * it is run, so a case that waits for the *client* to do something - to reject a
     * certificate, say - would otherwise be waiting on a server that has not spoken yet.
     */
    broker_pump(&probe->broker);
    (void)mesh_event_loop_run(&probe->loop, 5);
    probe->now_ms += step_ms;
    mesh_mqtt_proxy_tick(&probe->proxy, probe->now_ms);
}

static bool probe_until_state(struct proxy_probe *probe, enum mesh_mqtt_proxy_state state,
                              unsigned turns) {
    for (unsigned turn = 0U; turn < turns; ++turn) {
        if (mesh_mqtt_proxy_state(&probe->proxy) == state) {
            return true;
        }
        probe_turn(probe, 10U);
    }
    return mesh_mqtt_proxy_state(&probe->proxy) == state;
}

/* Turns until the broker has a whole packet, so a case never has to guess how many it takes. */
static bool probe_until_packet(struct proxy_probe *probe, struct mesh_mqtt_header *header,
                               const uint8_t **body, unsigned turns) {
    for (unsigned turn = 0U; turn < turns; ++turn) {
        if (broker_take(&probe->broker, header, body)) {
            return true;
        }
        probe_turn(probe, 10U);
    }
    return broker_take(&probe->broker, header, body);
}

static void probe_config(struct mesh_mqtt_proxy_config *config, uint16_t port) {
    memset(config, 0, sizeof *config);
    snprintf(config->address, sizeof config->address, "127.0.0.1:%u", (unsigned)port);
    snprintf(config->client_id, sizeof config->client_id, "meshclient-test");
}

static bool probe_start(struct proxy_probe *probe) {
    memset(probe, 0, sizeof *probe);
    broker_init(&probe->broker);
    if (mesh_event_loop_init(&probe->loop) != 0) {
        return false;
    }
    if (!broker_listen(&probe->broker)) {
        return false;
    }
    return mesh_mqtt_proxy_init(&probe->proxy, &probe->loop) == 0;
}

static void probe_stop(struct proxy_probe *probe) {
    mesh_mqtt_proxy_shutdown(&probe->proxy);
    broker_close(&probe->broker);
    mesh_event_loop_shutdown(&probe->loop);
}

/* Connects and gets as far as READY, which almost every case needs before it starts. */
static bool probe_connect(struct proxy_probe *probe) {
    struct mesh_mqtt_proxy_config config;
    probe_config(&config, probe->broker.port);
    if (mesh_mqtt_proxy_start(&probe->proxy, &config, probe_on_message, NULL, probe,
                              probe->now_ms) != 0) {
        return false;
    }
    struct mesh_mqtt_header header;
    const uint8_t *body = NULL;
    if (!probe_until_packet(probe, &header, &body, 100U) || header.type != MESH_MQTT_CONNECT) {
        return false;
    }
    broker_connack(&probe->broker, MESH_MQTT_CONNACK_ACCEPTED);
    return probe_until_state(probe, MESH_MQTT_PROXY_READY, 100U);
}

/* ------------------------------------------------------------------ getting connected */

MESH_TEST_CASE(mqtt_proxy_greets_a_broker, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }

    struct mesh_mqtt_proxy_config config;
    probe_config(&config, probe.broker.port);
    snprintf(config.username, sizeof config.username, "meshdev");
    snprintf(config.password, sizeof config.password, "swordfish");

    if (mesh_mqtt_proxy_start(&probe.proxy, &config, probe_on_message, NULL, &probe,
                              probe.now_ms) != 0) {
        record_failure(test_name, "the proxy did not start");
        goto cleanup;
    }

    struct mesh_mqtt_header header;
    const uint8_t *body = NULL;
    if (!probe_until_packet(&probe, &header, &body, 100U)) {
        record_failure(test_name, "the broker never saw a CONNECT");
        goto cleanup;
    }
    if (header.type != MESH_MQTT_CONNECT) {
        record_failure(test_name, "the first packet should be a CONNECT");
        goto cleanup;
    }
    /* Protocol name, level, then the flags byte: clean session with a username and a password. */
    static const uint8_t want_head[] = {0x00U, 0x04U, 'M', 'Q', 'T', 'T', 0x04U, 0xC2U};
    if (header.remaining < sizeof want_head || memcmp(body, want_head, sizeof want_head) != 0) {
        record_failure(test_name, "the CONNECT should announce 3.1.1 with credentials");
        goto cleanup;
    }
    /* The credentials the caller gave, in the packet, in the specified order. */
    if (!holds(body, header.remaining, "meshclient-test") ||
        !holds(body, header.remaining, "meshdev") || !holds(body, header.remaining, "swordfish")) {
        record_failure(test_name, "the CONNECT should carry the client id and credentials");
        goto cleanup;
    }

    /* Not connected until the broker says so - the socket being open is not acceptance. */
    if (mesh_mqtt_proxy_is_ready(&probe.proxy)) {
        record_failure(test_name, "a sent CONNECT is not an accepted one");
        goto cleanup;
    }
    broker_connack(&probe.broker, MESH_MQTT_CONNACK_ACCEPTED);
    if (!probe_until_state(&probe, MESH_MQTT_PROXY_READY, 100U)) {
        record_failure(test_name, "the proxy should reach READY after a CONNACK");
        goto cleanup;
    }
    if (mesh_mqtt_proxy_stats(&probe.proxy).connections != 1U) {
        record_failure(test_name, "an accepted connection should be counted");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

/*
 * A refused login is a different thing from an unreachable broker, and the proxy has to say so.
 *
 * It also has to keep backing off. The tempting bug is to treat a completed TCP connect as
 * progress and clear the failure count there - which turns a broker that rejects our password
 * into a five-second retry loop against somebody else's server, forever.
 */
MESH_TEST_CASE(mqtt_proxy_reports_a_refused_login, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }

    struct mesh_mqtt_proxy_config config;
    probe_config(&config, probe.broker.port);
    if (mesh_mqtt_proxy_start(&probe.proxy, &config, probe_on_message, NULL, &probe,
                              probe.now_ms) != 0) {
        record_failure(test_name, "the proxy did not start");
        goto cleanup;
    }

    struct mesh_mqtt_header header;
    const uint8_t *body = NULL;
    if (!probe_until_packet(&probe, &header, &body, 100U)) {
        record_failure(test_name, "the broker never saw a CONNECT");
        goto cleanup;
    }
    broker_connack(&probe.broker, MESH_MQTT_CONNACK_BAD_CREDENTIALS);

    if (!probe_until_state(&probe, MESH_MQTT_PROXY_WAITING, 100U)) {
        record_failure(test_name, "a refused login should end the attempt");
        goto cleanup;
    }
    const char *error = mesh_mqtt_proxy_last_error(&probe.proxy);
    if (error[0] == '\0' || strstr(error, "127.0.0.1") == NULL) {
        record_failure(test_name, "the refusal should name the broker, in words");
        goto cleanup;
    }
    if (mesh_mqtt_proxy_stats(&probe.proxy).connections != 0U) {
        record_failure(test_name, "a refused connection is not a connection");
        goto cleanup;
    }

    /* And it waits. Half the backoff is not long enough, which is what says the TCP connect did
       not quietly reset the count. */
    probe.now_ms += MESH_MQTT_BACKOFF_BASE_MS / 2U;
    mesh_mqtt_proxy_tick(&probe.proxy, probe.now_ms);
    if (mesh_mqtt_proxy_state(&probe.proxy) != MESH_MQTT_PROXY_WAITING) {
        record_failure(test_name, "the retry should wait out the backoff");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

MESH_TEST_CASE(mqtt_proxy_refuses_what_it_cannot_connect_to, unit) {
    struct mesh_mqtt_proxy proxy;
    struct mesh_mqtt_proxy_config config;
    probe_config(&config, 1883U);

    /* No loop: nothing to watch a socket with, so nothing is opened. */
    (void)mesh_mqtt_proxy_init(&proxy, NULL);
    if (mesh_mqtt_proxy_start(&proxy, &config, NULL, NULL, NULL, 0U) != -ENOTSUP) {
        record_failure(test_name, "a proxy with no loop should refuse to start");
        return;
    }
    mesh_mqtt_proxy_shutdown(&proxy);

    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "the loop did not start");
        return;
    }
    (void)mesh_mqtt_proxy_init(&proxy, &loop);

    /* An address that is not one, an empty address, and no client id. All refused before
       anything is opened, so the caller gets a return code rather than a connection that fails
       later for a reason nobody can read. */
    struct mesh_mqtt_proxy_config broken = config;
    snprintf(broken.address, sizeof broken.address, "[not-an-address");
    if (mesh_mqtt_proxy_start(&proxy, &broken, NULL, NULL, NULL, 0U) != -EINVAL) {
        record_failure(test_name, "a malformed address should be refused");
        goto cleanup;
    }
    broken = config;
    broken.address[0] = '\0';
    if (mesh_mqtt_proxy_start(&proxy, &broken, NULL, NULL, NULL, 0U) != -EINVAL) {
        record_failure(test_name, "an empty address should be refused");
        goto cleanup;
    }
    broken = config;
    broken.client_id[0] = '\0';
    if (mesh_mqtt_proxy_start(&proxy, &broken, NULL, NULL, NULL, 0U) != -EINVAL) {
        record_failure(test_name, "a missing client id should be refused");
        goto cleanup;
    }
    if (mesh_mqtt_proxy_state(&proxy) != MESH_MQTT_PROXY_OFF) {
        record_failure(test_name, "a refused start should leave the proxy off");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    mesh_mqtt_proxy_shutdown(&proxy);
    mesh_event_loop_shutdown(&loop);
}

/* ------------------------------------------------------------------ subscriptions */

MESH_TEST_CASE(mqtt_proxy_subscribes_one_filter_at_a_time, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    if (mesh_mqtt_proxy_subscribe(&probe.proxy, "msh/2/e/LongFast/#") != 0 ||
        mesh_mqtt_proxy_subscribe(&probe.proxy, "msh/2/e/Secondary/#") != 0) {
        record_failure(test_name, "filters added before a connection should be held");
        goto cleanup;
    }
    /* Adding the same one twice is not a failure: the caller re-derives the whole set whenever
       the radio's channels change, and having to diff it first would be the caller's bug. */
    if (mesh_mqtt_proxy_subscribe(&probe.proxy, "msh/2/e/LongFast/#") != -EEXIST) {
        record_failure(test_name, "a repeated filter should be reported as already held");
        goto cleanup;
    }

    if (!probe_connect(&probe)) {
        record_failure(test_name, "the proxy did not connect");
        goto cleanup;
    }

    struct mesh_mqtt_header header;
    const uint8_t *body = NULL;
    if (!probe_until_packet(&probe, &header, &body, 100U)) {
        record_failure(test_name, "the broker never saw a SUBSCRIBE");
        goto cleanup;
    }
    if (header.type != MESH_MQTT_SUBSCRIBE || header.flags != 0x02U) {
        record_failure(test_name, "a SUBSCRIBE carries the 0b0010 flags nibble");
        goto cleanup;
    }
    if (!holds(body, header.remaining, "LongFast")) {
        record_failure(test_name, "the first filter should go out first");
        goto cleanup;
    }

    /* The second must NOT be on the wire yet: one at a time is what makes a refusal
       attributable to the filter that caused it. */
    struct mesh_mqtt_header second;
    const uint8_t *second_body = NULL;
    probe_turn(&probe, 10U);
    if (broker_take(&probe.broker, &second, &second_body)) {
        record_failure(test_name, "the second filter should wait for the first SUBACK");
        goto cleanup;
    }

    const uint16_t id = (uint16_t)(((uint16_t)body[0] << 8) | (uint16_t)body[1]);
    broker_suback(&probe.broker, id, 0U);
    if (!probe_until_packet(&probe, &second, &second_body, 100U)) {
        record_failure(test_name, "the SUBACK should release the next filter");
        goto cleanup;
    }
    if (second.type != MESH_MQTT_SUBSCRIBE || !holds(second_body, second.remaining, "Secondary")) {
        record_failure(test_name, "the second filter should follow the first");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

/*
 * A filter the broker refuses costs that filter and nothing else.
 *
 * Dropping the connection over one would lose every other subscription with it, and a broker
 * with topic ACLs permitting some channels and not others is an ordinary configuration rather
 * than a broken one.
 */
MESH_TEST_CASE(mqtt_proxy_survives_a_refused_filter, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    if (mesh_mqtt_proxy_subscribe(&probe.proxy, "msh/2/e/Forbidden/#") != 0 ||
        mesh_mqtt_proxy_subscribe(&probe.proxy, "msh/2/e/Allowed/#") != 0 ||
        !probe_connect(&probe)) {
        record_failure(test_name, "the proxy did not connect with its filters");
        goto cleanup;
    }

    struct mesh_mqtt_header header;
    const uint8_t *body = NULL;
    if (!probe_until_packet(&probe, &header, &body, 100U)) {
        record_failure(test_name, "the broker never saw a SUBSCRIBE");
        goto cleanup;
    }
    const uint16_t id = (uint16_t)(((uint16_t)body[0] << 8) | (uint16_t)body[1]);
    broker_suback(&probe.broker, id, MESH_MQTT_SUBACK_FAILURE);

    if (!probe_until_packet(&probe, &header, &body, 100U) || header.type != MESH_MQTT_SUBSCRIBE ||
        !holds(body, header.remaining, "Allowed")) {
        record_failure(test_name, "a refused filter should not stop the next one");
        goto cleanup;
    }
    if (!mesh_mqtt_proxy_is_ready(&probe.proxy)) {
        record_failure(test_name, "a refused filter should not drop the connection");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

/* ------------------------------------------------------------------ carrying traffic */

MESH_TEST_CASE(mqtt_proxy_publishes_what_the_radio_gives_it, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }

    /* Before a connection, a publish is refused and counted. This is the ordinary state of the
       thing while a link is down, and the radio will keep offering - so it is a return code,
       not a log line. */
    static const uint8_t payload[] = {0x01U, 0x02U, 0x03U, 0x04U};
    if (mesh_mqtt_proxy_publish(&probe.proxy, "msh/2/e/LongFast/!a", payload, sizeof payload,
                                false) != -ENOTCONN) {
        record_failure(test_name, "a publish with no broker should be refused");
        goto cleanup;
    }
    if (mesh_mqtt_proxy_stats(&probe.proxy).dropped != 1U) {
        record_failure(test_name, "a refused publish should be counted as dropped");
        goto cleanup;
    }

    if (!probe_connect(&probe)) {
        record_failure(test_name, "the proxy did not connect");
        goto cleanup;
    }
    if (mesh_mqtt_proxy_publish(&probe.proxy, "msh/2/e/LongFast/!a", payload, sizeof payload,
                                false) != 0) {
        record_failure(test_name, "a publish on a live connection should be taken");
        goto cleanup;
    }

    struct mesh_mqtt_header header;
    const uint8_t *body = NULL;
    if (!probe_until_packet(&probe, &header, &body, 100U)) {
        record_failure(test_name, "the broker never saw the PUBLISH");
        goto cleanup;
    }
    struct mesh_mqtt_incoming message;
    if (header.type != MESH_MQTT_PUBLISH ||
        mesh_mqtt_decode_publish(header.flags, body, header.remaining, &message) != 0) {
        record_failure(test_name, "the broker should see a well-formed PUBLISH");
        goto cleanup;
    }
    if (message.topic_len != 19U || memcmp(message.topic, "msh/2/e/LongFast/!a", 19U) != 0 ||
        message.payload_len != sizeof payload ||
        memcmp(message.payload, payload, sizeof payload) != 0) {
        record_failure(test_name, "the topic and payload should arrive unchanged");
        goto cleanup;
    }
    if (message.qos != 0U || message.retained) {
        record_failure(test_name, "a publish should go out at QoS 0, not retained");
        goto cleanup;
    }

    /*
     * A wildcard is legal in a filter and forbidden in a published topic. A broker's answer to
     * one is to close the connection rather than to say so, which would present as a link that
     * flaps whenever one particular channel had traffic - so it is refused here instead.
     */
    if (mesh_mqtt_proxy_publish(&probe.proxy, "msh/2/e/#", payload, sizeof payload, false) !=
        -EINVAL) {
        record_failure(test_name, "a wildcard topic should be refused");
        goto cleanup;
    }
    if (!mesh_mqtt_proxy_is_ready(&probe.proxy)) {
        record_failure(test_name, "a refused publish should not drop the connection");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

MESH_TEST_CASE(mqtt_proxy_delivers_what_the_broker_sends, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    if (!probe_connect(&probe)) {
        record_failure(test_name, "the proxy did not connect");
        goto cleanup;
    }

    uint8_t packet[256];
    static const uint8_t payload[] = {0xC0U, 0xFFU, 0xEEU};
    const int len = mesh_mqtt_encode_publish(packet, sizeof packet, "msh/2/e/LongFast/!b", payload,
                                             sizeof payload, false);
    if (len < 0) {
        record_failure(test_name, "the fixture could not build a PUBLISH");
        goto cleanup;
    }
    broker_send(&probe.broker, packet, (size_t)len);

    for (unsigned turn = 0U; turn < 100U && probe.messages == 0U; ++turn) {
        probe_turn(&probe, 10U);
    }
    if (probe.messages != 1U) {
        record_failure(test_name, "the message should have been delivered once");
        goto cleanup;
    }
    if (strcmp(probe.last_topic, "msh/2/e/LongFast/!b") != 0 ||
        probe.last_payload_len != sizeof payload ||
        memcmp(probe.last_payload, payload, sizeof payload) != 0) {
        record_failure(test_name, "the topic and payload should arrive unchanged");
        goto cleanup;
    }
    if (mesh_mqtt_proxy_stats(&probe.proxy).received != 1U) {
        record_failure(test_name, "a delivered message should be counted");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

/*
 * A message too large to forward is counted off the stream, not buffered - and what follows it
 * still arrives.
 *
 * This is the case the whole header/body split in the codec exists for. A broker may retain a
 * message far larger than anything a radio could accept; the bytes still have to be consumed in
 * order, because an MQTT stream has no resynchronisation and every packet after a mis-framed one
 * is read at the wrong offset. The second, ordinary message arriving intact is the proof that
 * the skip counted correctly - one byte out either way and it would decode as nonsense.
 */
MESH_TEST_CASE(mqtt_proxy_skips_an_oversized_message, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    if (!probe_connect(&probe)) {
        record_failure(test_name, "the proxy did not connect");
        goto cleanup;
    }

    /* Comfortably past MESH_MQTT_PACKET_MAX, and past a single read besides, so the skip has to
       survive being spread over several of them. */
    static uint8_t big[6000];
    memset(big, 0x5AU, sizeof big);
    uint8_t oversized[sizeof big + 64U];
    const int big_len = mesh_mqtt_encode_publish(oversized, sizeof oversized, "msh/2/e/big", big,
                                                 sizeof big, false);
    uint8_t small[128];
    static const uint8_t payload[] = {0x11U, 0x22U};
    const int small_len = mesh_mqtt_encode_publish(small, sizeof small, "msh/2/e/small", payload,
                                                   sizeof payload, false);
    if (big_len < 0 || small_len < 0) {
        record_failure(test_name, "the fixture could not build its packets");
        goto cleanup;
    }
    broker_send(&probe.broker, oversized, (size_t)big_len);
    broker_send(&probe.broker, small, (size_t)small_len);

    for (unsigned turn = 0U; turn < 400U && probe.messages == 0U; ++turn) {
        probe_turn(&probe, 10U);
    }

    if (probe.messages != 1U) {
        record_failure(test_name, "exactly the message that fits should be delivered");
        goto cleanup;
    }
    if (strcmp(probe.last_topic, "msh/2/e/small") != 0 ||
        probe.last_payload_len != sizeof payload) {
        record_failure(test_name, "the message after the skipped one should be intact");
        goto cleanup;
    }
    if (mesh_mqtt_proxy_stats(&probe.proxy).skipped == 0U) {
        record_failure(test_name, "the skipped message should be counted");
        goto cleanup;
    }
    if (!mesh_mqtt_proxy_is_ready(&probe.proxy)) {
        record_failure(test_name, "an oversized message should not drop the connection");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

/* ------------------------------------------------------------------ keeping it alive */

MESH_TEST_CASE(mqtt_proxy_pings_a_quiet_broker, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    if (!probe_connect(&probe)) {
        record_failure(test_name, "the proxy did not connect");
        goto cleanup;
    }

    /* Nothing has been said in either direction for longer than the ping interval. The broker
       drops a client that has gone silent, and a mesh can be quiet for hours. */
    probe.now_ms += MESH_MQTT_PING_INTERVAL_MS + 1U;
    struct mesh_mqtt_header header;
    const uint8_t *body = NULL;
    if (!probe_until_packet(&probe, &header, &body, 50U) || header.type != MESH_MQTT_PINGREQ) {
        record_failure(test_name, "a quiet connection should send a PINGREQ");
        goto cleanup;
    }
    if (header.remaining != 0U) {
        record_failure(test_name, "a PINGREQ has no body");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

/*
 * A broker that stops answering without closing anything.
 *
 * This is the ordinary case on a handheld, not the exotic one: a Brick carried out of WiFi range
 * leaves a socket that is open, writable, and connected to nobody. Nothing but the clock
 * notices.
 */
MESH_TEST_CASE(mqtt_proxy_gives_up_on_a_silent_broker, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    if (!probe_connect(&probe)) {
        record_failure(test_name, "the proxy did not connect");
        goto cleanup;
    }

    probe.now_ms += MESH_MQTT_SILENCE_TIMEOUT_MS + 1U;
    mesh_mqtt_proxy_tick(&probe.proxy, probe.now_ms);
    if (mesh_mqtt_proxy_state(&probe.proxy) != MESH_MQTT_PROXY_WAITING) {
        record_failure(test_name, "a silent broker should end the connection");
        goto cleanup;
    }
    if (mesh_mqtt_proxy_last_error(&probe.proxy)[0] == '\0') {
        record_failure(test_name, "the timeout should say something");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

/* A broker that hangs up is noticed, and the connection is remade once the backoff expires. */
MESH_TEST_CASE(mqtt_proxy_reconnects_after_a_hangup, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    if (!probe_connect(&probe)) {
        record_failure(test_name, "the proxy did not connect");
        goto cleanup;
    }

    close(probe.broker.client);
    probe.broker.client = -1;
    if (!probe_until_state(&probe, MESH_MQTT_PROXY_WAITING, 100U)) {
        record_failure(test_name, "a closed connection should be noticed");
        goto cleanup;
    }

    /* Past the first backoff, which is the base delay because this is the first failure. */
    probe.now_ms += MESH_MQTT_BACKOFF_BASE_MS + 1U;
    mesh_mqtt_proxy_tick(&probe.proxy, probe.now_ms);

    struct mesh_mqtt_header header;
    const uint8_t *body = NULL;
    if (!probe_until_packet(&probe, &header, &body, 100U) || header.type != MESH_MQTT_CONNECT) {
        record_failure(test_name, "the proxy should try again after the backoff");
        goto cleanup;
    }
    broker_connack(&probe.broker, MESH_MQTT_CONNACK_ACCEPTED);
    if (!probe_until_state(&probe, MESH_MQTT_PROXY_READY, 100U)) {
        record_failure(test_name, "the second attempt should connect");
        goto cleanup;
    }
    if (mesh_mqtt_proxy_stats(&probe.proxy).connections != 2U) {
        record_failure(test_name, "both connections should be counted");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

/*
 * A stop() is deliberate and says so on the wire.
 *
 * Without the DISCONNECT the broker sees a client that stopped answering, which is a different
 * thing: it holds the session open until the keepalive expires, and any will message it had been
 * told about would fire.
 */
MESH_TEST_CASE(mqtt_proxy_says_goodbye, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    if (!probe_connect(&probe)) {
        record_failure(test_name, "the proxy did not connect");
        goto cleanup;
    }

    mesh_mqtt_proxy_stop(&probe.proxy);
    if (mesh_mqtt_proxy_state(&probe.proxy) != MESH_MQTT_PROXY_OFF) {
        record_failure(test_name, "a stopped proxy should be off");
        goto cleanup;
    }

    struct mesh_mqtt_header header;
    const uint8_t *body = NULL;
    if (!probe_until_packet(&probe, &header, &body, 20U) || header.type != MESH_MQTT_DISCONNECT) {
        record_failure(test_name, "a deliberate stop should send a DISCONNECT");
        goto cleanup;
    }

    /* And it stays off: no retry is scheduled for something the caller turned off. */
    probe.now_ms += MESH_MQTT_BACKOFF_MAX_MS;
    mesh_mqtt_proxy_tick(&probe.proxy, probe.now_ms);
    if (mesh_mqtt_proxy_state(&probe.proxy) != MESH_MQTT_PROXY_OFF) {
        record_failure(test_name, "a stopped proxy should not reconnect itself");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

/*
 * A broker that answers with something only a client sends.
 *
 * Not a broker being eccentric: it means the stream is being read at the wrong offset, and every
 * packet after it would decode as plausible nonsense. Dropping the connection is the only
 * recovery an MQTT stream has.
 */
MESH_TEST_CASE(mqtt_proxy_drops_a_desynchronised_stream, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    if (!probe_connect(&probe)) {
        record_failure(test_name, "the proxy did not connect");
        goto cleanup;
    }

    /* A CONNECT, from the server. */
    static const uint8_t wrong_way[] = {0x10U, 0x00U};
    broker_send(&probe.broker, wrong_way, sizeof wrong_way);

    if (!probe_until_state(&probe, MESH_MQTT_PROXY_WAITING, 100U)) {
        record_failure(test_name, "a client-to-server packet from a broker should be fatal");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

/* ------------------------------------------------------------------ TLS */

#ifdef MESHCLIENT_HAVE_TLS

/*
 * A real handshake against a real broker, and then MQTT over it.
 *
 * The certificate carries `IP:127.0.0.1` in its subjectAltName, which is what lets this connect
 * to a literal rather than standing the fixture on both loopback families to resolve a name.
 * The name checked is the one the caller typed - that is the whole point of verifying against
 * `proxy->host` rather than against whatever the address turned out to be.
 */
MESH_TEST_CASE(mqtt_proxy_connects_over_tls, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    probe.broker.tls = true;

    char bundle[128];
    snprintf(bundle, sizeof bundle, "/tmp/meshclient-test-ca-%u.pem", (unsigned)getpid());
    if (!write_pem(bundle, broker_cert_pem)) {
        record_failure(test_name, "could not write the CA bundle");
        goto cleanup;
    }
    mesh_mqtt_proxy_set_ca_bundle(&probe.proxy, bundle);

    struct mesh_mqtt_proxy_config config;
    probe_config(&config, probe.broker.port);
    config.tls_enabled = true;
    if (mesh_mqtt_proxy_start(&probe.proxy, &config, probe_on_message, NULL, &probe,
                              probe.now_ms) != 0) {
        record_failure(test_name, "the proxy did not start");
        goto cleanup;
    }

    struct mesh_mqtt_header header;
    const uint8_t *body = NULL;
    if (!probe_until_packet(&probe, &header, &body, 400U) || header.type != MESH_MQTT_CONNECT) {
        record_failure(test_name, "the CONNECT should arrive through the TLS session");
        goto cleanup;
    }
    broker_connack(&probe.broker, MESH_MQTT_CONNACK_ACCEPTED);
    if (!probe_until_state(&probe, MESH_MQTT_PROXY_READY, 400U)) {
        record_failure(test_name, "the proxy should reach READY over TLS");
        goto cleanup;
    }

    /* And traffic goes both ways over it, which is what says the session is a stream and not
       just a handshake that completed. */
    static const uint8_t payload[] = {0xA1U, 0xB2U, 0xC3U};
    if (mesh_mqtt_proxy_publish(&probe.proxy, "msh/2/e/tls/!c", payload, sizeof payload, false) !=
        0) {
        record_failure(test_name, "a publish over TLS should be taken");
        goto cleanup;
    }
    struct mesh_mqtt_incoming message;
    if (!probe_until_packet(&probe, &header, &body, 400U) || header.type != MESH_MQTT_PUBLISH ||
        mesh_mqtt_decode_publish(header.flags, body, header.remaining, &message) != 0 ||
        message.payload_len != sizeof payload ||
        memcmp(message.payload, payload, sizeof payload) != 0) {
        record_failure(test_name, "the published payload should arrive intact over TLS");
        goto cleanup;
    }

    uint8_t inbound[128];
    static const uint8_t reply[] = {0xD4U, 0xE5U};
    const int len = mesh_mqtt_encode_publish(inbound, sizeof inbound, "msh/2/e/tls/!d", reply,
                                             sizeof reply, false);
    if (len < 0) {
        record_failure(test_name, "the fixture could not build a PUBLISH");
        goto cleanup;
    }
    broker_send(&probe.broker, inbound, (size_t)len);
    for (unsigned turn = 0U; turn < 400U && probe.messages == 0U; ++turn) {
        probe_turn(&probe, 10U);
    }
    if (probe.messages != 1U || probe.last_payload_len != sizeof reply ||
        memcmp(probe.last_payload, reply, sizeof reply) != 0) {
        record_failure(test_name, "an inbound message should arrive over TLS");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    (void)remove(bundle);
    probe_stop(&probe);
}

/*
 * The other half, and the one that matters: a certificate the bundle does not vouch for is
 * refused.
 *
 * Without this case the one above proves nothing about verification - a client that trusts
 * everything passes it just as well. The broker presents the same certificate; the only thing
 * changed is that the bundle holds an unrelated one, and the connection must not be made.
 */
MESH_TEST_CASE(mqtt_proxy_refuses_an_unknown_certificate, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    probe.broker.tls = true;

    char bundle[128];
    snprintf(bundle, sizeof bundle, "/tmp/meshclient-test-stranger-%u.pem", (unsigned)getpid());
    if (!write_pem(bundle, stranger_cert_pem)) {
        record_failure(test_name, "could not write the CA bundle");
        goto cleanup;
    }
    mesh_mqtt_proxy_set_ca_bundle(&probe.proxy, bundle);

    struct mesh_mqtt_proxy_config config;
    probe_config(&config, probe.broker.port);
    config.tls_enabled = true;
    if (mesh_mqtt_proxy_start(&probe.proxy, &config, probe_on_message, NULL, &probe,
                              probe.now_ms) != 0) {
        record_failure(test_name, "the proxy did not start");
        goto cleanup;
    }

    if (!probe_until_state(&probe, MESH_MQTT_PROXY_WAITING, 400U)) {
        record_failure(test_name, "an unverifiable certificate should end the attempt");
        goto cleanup;
    }
    if (mesh_mqtt_proxy_is_ready(&probe.proxy)) {
        record_failure(test_name, "an unverifiable certificate must not connect");
        goto cleanup;
    }
    /* And it says so in words rather than as a number, which is the reason MBEDTLS_ERROR_C is
       left enabled. */
    const char *error = mesh_mqtt_proxy_last_error(&probe.proxy);
    if (error[0] == '\0' || strstr(error, "127.0.0.1") == NULL) {
        record_failure(test_name, "the refusal should name the broker and say why");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    (void)remove(bundle);
    probe_stop(&probe);
}

/*
 * A bundle that is missing is a refusal, not a downgrade.
 *
 * The Brick has no system certificate store, so "no bundle" is a state this reaches in the
 * field rather than a theoretical one - which is exactly why it must not be the state in which
 * verification quietly stops happening.
 */
MESH_TEST_CASE(mqtt_proxy_will_not_do_tls_without_a_bundle, unit) {
    struct proxy_probe probe;
    if (!probe_start(&probe)) {
        record_failure(test_name, "the harness did not start");
        return;
    }
    probe.broker.tls = true;
    mesh_mqtt_proxy_set_ca_bundle(&probe.proxy, NULL);

    struct mesh_mqtt_proxy_config config;
    probe_config(&config, probe.broker.port);
    config.tls_enabled = true;
    if (mesh_mqtt_proxy_start(&probe.proxy, &config, probe_on_message, NULL, &probe,
                              probe.now_ms) != 0) {
        record_failure(test_name, "the proxy did not start");
        goto cleanup;
    }
    if (!probe_until_state(&probe, MESH_MQTT_PROXY_WAITING, 400U)) {
        record_failure(test_name, "TLS with no bundle should end the attempt");
        goto cleanup;
    }
    if (mesh_mqtt_proxy_is_ready(&probe.proxy)) {
        record_failure(test_name, "TLS with no bundle must not connect");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    probe_stop(&probe);
}

#endif /* MESHCLIENT_HAVE_TLS */
