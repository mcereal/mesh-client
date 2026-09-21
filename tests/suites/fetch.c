#define _POSIX_C_SOURCE 200809L

/*
 * The fetcher, against a real HTTPS server on loopback (support/https_fixture.h).
 *
 * What is under test is everything between a URL and an outcome: the handshake and the
 * certificate check, a redirect to another host, the three ways a body can be framed and the one
 * way a close can lie about it, a range the server ignored, a file that must not appear for a
 * 404, and the one thing about the API that is not obvious - that a caller may start its next
 * request from inside the completion of the last. The codec underneath has its own suite
 * (tests/suites/http.c); this one is about what the fetcher decides with what it reads.
 */

#include "framework/mesh_test.h"

#include "inkwell/runtime/loop.h"
#include "mesh/core/fetch.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

MESH_TEST_CASE(fetch_reads_a_content_length_out_of_a_head, unit) {
    static const char k_head[] = "HTTP/1.1 200 OK\r\n"
                                 "Accept-Ranges: bytes\r\n"
                                 "content-length: 46259773\r\n\r\n";
    uint64_t size = 0U;
    MESH_TEST_FAIL_IF(!mesh_fetch_content_length(k_head, 0U, &size) || size != 46259773U,
                      "the length should be read case-blind");
    MESH_TEST_FAIL_IF(mesh_fetch_content_length("HTTP/1.1 200 OK\r\n\r\n", 0U, &size),
                      "no header is no length");
    MESH_TEST_FAIL_IF(mesh_fetch_content_length("Content-Length: lots\r\n", 0U, &size),
                      "a length that is not a number is none");
    record_success(test_name);
}

MESH_TEST_CASE(fetch_refuses_what_it_cannot_start, unit) {
    struct mesh_fetch fetch;
    MESH_TEST_FAIL_IF(mesh_fetch_init(&fetch, NULL) != 0, "a loopless fetcher should init");
    MESH_TEST_FAIL_IF(mesh_fetch_available(&fetch), "and report itself unavailable");
    mesh_fetch_shutdown(&fetch);
    record_success(test_name);
}

#ifdef MESHCLIENT_HAVE_TLS

#include "support/https_fixture.h"

#include "mesh/core/ca_roots.h"
#include "mesh/core/tls_client.h"

#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ the server */

static const char k_document[] = "{\"tag_name\":\"v1.2.3\"}";

/* Every route a case in this file asks for. Runs in the fixture's child. */
static void fetch_serve(void *userdata, const struct https_fixture_request *request,
                        struct https_fixture_conn *conn) {
    (void)userdata;
    const char *const target = request->target;
    if (strcmp(target, "/doc") == 0) {
        https_fixture_reply(conn, 200, "Content-Type: application/json\r\n", k_document,
                            sizeof k_document - 1U);
    } else if (strcmp(target, "/big") == 0) {
        static char big[4096];
        memset(big, 'x', sizeof big);
        https_fixture_reply(conn, 200, NULL, big, sizeof big);
    } else if (strcmp(target, "/release") == 0) {
        /* GitHub's shape: the release URL is a 302 to another host, and the 302 carries a
           length of its own that is not the file's. */
        https_fixture_reply(
            conn, 302, "Location: https://objects.githubusercontent.com/asset?sig=1\r\n", NULL, 0U);
    } else if (strcmp(target, "/asset?sig=1") == 0) {
        if (request->ranged) {
            https_fixture_printf(conn,
                                 "HTTP/1.1 206 Partial\r\nContent-Range: bytes %llu-%llu/1000\r\n"
                                 "Content-Length: %llu\r\n\r\n",
                                 (unsigned long long)request->first,
                                 (unsigned long long)request->last,
                                 (unsigned long long)(request->last - request->first + 1U));
            for (uint64_t i = request->first; i <= request->last; ++i) {
                const char c = (char)('a' + i % 26U);
                https_fixture_send(conn, &c, 1U);
            }
        } else {
            static char asset[1000];
            for (size_t i = 0U; i < sizeof asset; ++i) {
                asset[i] = (char)('a' + i % 26U);
            }
            https_fixture_reply(conn, 200, "Accept-Ranges: bytes\r\n", asset, sizeof asset);
        }
    } else if (strcmp(target, "/whole") == 0) {
        /* A server that ignores a range and sends the file. */
        https_fixture_reply(conn, 200, NULL, "the whole file", 14U);
    } else if (strcmp(target, "/loop") == 0) {
        https_fixture_reply(conn, 302, "Location: /loop\r\n", NULL, 0U);
    } else if (strcmp(target, "/plain") == 0) {
        https_fixture_reply(conn, 301, "Location: http://github.com/doc\r\n", NULL, 0U);
    } else if (strcmp(target, "/chunked") == 0) {
        https_fixture_printf(conn, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                   "5\r\nhello\r\n7;ext=1\r\n, world\r\n0\r\n\r\n");
    } else if (strcmp(target, "/until-close") == 0) {
        https_fixture_printf(conn, "HTTP/1.1 200 OK\r\n\r\nall of it");
    } else if (strcmp(target, "/until-cut") == 0) {
        https_fixture_printf(conn, "HTTP/1.1 200 OK\r\n\r\nsome of it");
        https_fixture_cut(conn);
    } else if (strcmp(target, "/short") == 0) {
        https_fixture_printf(conn, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nten bytes!");
        https_fixture_cut(conn);
    } else if (strcmp(target, "/garbage") == 0) {
        https_fixture_printf(conn, "SSH-2.0-OpenSSH\r\n\r\n");
    } else if (strcmp(target, "/slow") == 0) {
        sleep(30);
    } else {
        https_fixture_reply(conn, 404, NULL, "not here", 8U);
    }
}

/* ------------------------------------------------------------------ the client */

struct fetch_probe {
    struct mesh_fetch *fetch;
    enum mesh_fetch_outcome outcome[4];
    int status[4];
    char body[4][1024];
    size_t len[4];
    unsigned calls;
    /* When set, the first completion starts this URL - the chained case. */
    const char *chain_url;
};

static void probe_record(void *userdata, const struct mesh_fetch_result *result);

static void probe_record(void *userdata, const struct mesh_fetch_result *result) {
    struct fetch_probe *const probe = (struct fetch_probe *)userdata;
    if (probe->calls < 4U) {
        const unsigned slot = probe->calls;
        probe->outcome[slot] = result->outcome;
        probe->status[slot] = result->status;
        probe->len[slot] = result->len;
        snprintf(probe->body[slot], sizeof probe->body[slot], "%s",
                 result->body != NULL ? result->body : "");
    }
    probe->calls++;

    if (probe->chain_url != NULL) {
        const char *const url = probe->chain_url;
        probe->chain_url = NULL;
        const struct mesh_fetch_request next = {
            .url = url,
            .timeout_ms = 5000U,
            .on_done = probe_record,
            .userdata = probe,
        };
        (void)mesh_fetch_start(probe->fetch, &next, 0U);
    }
}

struct fetch_harness {
    struct https_fixture server;
    struct inkwell_loop loop;
    struct mesh_fetch fetch;
    struct fetch_probe probe;
    bool loop_up;
    bool fetch_up;
};

static bool harness_start(struct fetch_harness *h) {
    memset(h, 0, sizeof *h);
    if (!https_fixture_start(&h->server, fetch_serve, NULL)) {
        return false;
    }
    if (inkwell_loop_init(&h->loop) != 0) {
        return false;
    }
    h->loop_up = true;
    if (mesh_fetch_init(&h->fetch, &h->loop) != 0) {
        return false;
    }
    h->fetch_up = true;
    https_fixture_attach(&h->server, &h->fetch);
    h->probe.fetch = &h->fetch;
    return true;
}

static void harness_stop(struct fetch_harness *h) {
    if (h->fetch_up) {
        mesh_fetch_shutdown(&h->fetch);
    }
    if (h->loop_up) {
        inkwell_loop_shutdown(&h->loop);
    }
    https_fixture_stop(&h->server);
}

/* Pumps the loop until the probe has seen `wanted` completions, or the budget runs out. */
static bool harness_wait(struct fetch_harness *h, unsigned wanted) {
    for (int turn = 0; turn < 1000 && h->probe.calls < wanted; ++turn) {
        (void)inkwell_loop_run(&h->loop, 10);
        mesh_fetch_tick(&h->fetch, 0U);
    }
    return h->probe.calls >= wanted;
}

/* Starts one request and waits for it. False when it did not start or did not finish. */
static bool harness_fetch(struct fetch_harness *h, const struct mesh_fetch_request *request) {
    struct mesh_fetch_request copy = *request;
    copy.on_done = probe_record;
    copy.userdata = &h->probe;
    if (copy.timeout_ms == 0U) {
        copy.timeout_ms = 5000U;
    }
    const unsigned before = h->probe.calls;
    return mesh_fetch_start(&h->fetch, &copy, 0U) == 0 && harness_wait(h, before + 1U);
}

/* ------------------------------------------------------------------ cases */

MESH_TEST_CASE(fetch_reads_a_body_and_names_a_failure, unit) {
    struct fetch_harness h;
    const char *failure = NULL;
    if (!harness_start(&h)) {
        failure = "the harness did not start";
        goto cleanup;
    }

    const struct mesh_fetch_request doc = {.url = "https://api.github.com/doc"};
    struct mesh_fetch_request with_callback = doc;
    with_callback.on_done = probe_record;
    with_callback.userdata = &h.probe;
    if (mesh_fetch_start(&h.fetch, &with_callback, 0U) != 0) {
        failure = "a first request should start";
        goto cleanup;
    }
    if (mesh_fetch_start(&h.fetch, &with_callback, 0U) != -EBUSY) {
        failure = "a second request while one is in flight should be refused";
        goto cleanup;
    }
    if (h.probe.calls != 0U) {
        failure = "the callback must never run from inside start()";
        goto cleanup;
    }
    if (!harness_wait(&h, 1U) || h.probe.outcome[0] != MESH_FETCH_OK || h.probe.status[0] != 200 ||
        strcmp(h.probe.body[0], k_document) != 0 || h.probe.len[0] != sizeof k_document - 1U) {
        failure = "the body should arrive whole";
        goto cleanup;
    }
    if (mesh_fetch_busy(&h.fetch)) {
        failure = "the fetcher should be idle once its completion has run";
        goto cleanup;
    }

    const struct mesh_fetch_request missing = {.url = "https://api.github.com/nothing"};
    if (!harness_fetch(&h, &missing) || h.probe.outcome[1] != MESH_FETCH_HTTP_STATUS ||
        h.probe.status[1] != 404 || h.probe.body[1][0] != '\0') {
        failure = "a 404 should be reported as one, with no body";
        goto cleanup;
    }

    const struct mesh_fetch_request garbage = {.url = "https://api.github.com/garbage"};
    if (!harness_fetch(&h, &garbage) || h.probe.outcome[2] != MESH_FETCH_PROTOCOL) {
        failure = "a reply that is not HTTP should be refused";
        goto cleanup;
    }

    char log[1024];
    https_fixture_requests(&h.server, log, sizeof log);
    if (strstr(log, "GET api.github.com /doc\n") == NULL) {
        failure = "the request should name the URL's host, not the address it went to";
        goto cleanup;
    }

cleanup:
    harness_stop(&h);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * A release asset, the way GitHub serves one: a 302 from github.com to another host, where the
 * file is. The range goes to both hops, because the second is the one that has the file.
 */
MESH_TEST_CASE(fetch_follows_a_redirect_to_another_host, unit) {
    struct fetch_harness h;
    const char *failure = NULL;
    if (!harness_start(&h)) {
        failure = "the harness did not start";
        goto cleanup;
    }

    const struct mesh_fetch_request ranged = {
        .url = "https://github.com/release",
        .headers = {"Range: bytes=26-30"},
    };
    if (!harness_fetch(&h, &ranged) || h.probe.outcome[0] != MESH_FETCH_OK ||
        h.probe.status[0] != 206 || strcmp(h.probe.body[0], "abcde") != 0) {
        failure = "the range should arrive from the host the redirect named";
        goto cleanup;
    }
    char log[1024];
    https_fixture_requests(&h.server, log, sizeof log);
    if (strcmp(log, "GET github.com /release 26-30\n"
                    "GET objects.githubusercontent.com /asset?sig=1 26-30\n") != 0) {
        failure = "both hops should carry the range, the second to the redirect's host";
        goto cleanup;
    }

    /* The HEAD a zip download starts with: the length is the file's, not the 302's. */
    const struct mesh_fetch_request head = {
        .url = "https://github.com/release",
        .method = MESH_FETCH_HEAD,
    };
    uint64_t size = 0U;
    if (!harness_fetch(&h, &head) || h.probe.outcome[1] != MESH_FETCH_OK ||
        !mesh_fetch_content_length(h.probe.body[1], h.probe.len[1], &size) || size != 1000U) {
        failure = "a HEAD should answer with the final hop's headers";
        goto cleanup;
    }

cleanup:
    harness_stop(&h);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

MESH_TEST_CASE(fetch_refuses_redirects_it_should_not_follow, unit) {
    struct fetch_harness h;
    const char *failure = NULL;
    if (!harness_start(&h)) {
        failure = "the harness did not start";
        goto cleanup;
    }

    const struct mesh_fetch_request plain = {.url = "https://github.com/plain"};
    if (!harness_fetch(&h, &plain) || h.probe.outcome[0] != MESH_FETCH_PROTOCOL) {
        failure = "a redirect off https should be refused, not followed";
        goto cleanup;
    }
    const struct mesh_fetch_request loop = {.url = "https://github.com/loop"};
    if (!harness_fetch(&h, &loop) || h.probe.outcome[1] != MESH_FETCH_PROTOCOL) {
        failure = "a redirect loop should end at the cap";
        goto cleanup;
    }
    char log[2048];
    https_fixture_requests(&h.server, log, sizeof log);
    unsigned loops = 0U;
    for (const char *at = strstr(log, "/loop\n"); at != NULL; at = strstr(at + 1, "/loop\n")) {
        loops++;
    }
    if (loops != MESH_FETCH_REDIRECTS_MAX + 1U) {
        failure = "the cap should be the first request and MESH_FETCH_REDIRECTS_MAX more";
        goto cleanup;
    }

cleanup:
    harness_stop(&h);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * The three framings, and the one way a close lies. A body with neither a length nor chunking
 * ends where the connection does - which a cut connection also does, so only a close_notify
 * makes it whole.
 */
MESH_TEST_CASE(fetch_knows_where_a_body_ends, unit) {
    struct fetch_harness h;
    const char *failure = NULL;
    if (!harness_start(&h)) {
        failure = "the harness did not start";
        goto cleanup;
    }

    const struct mesh_fetch_request chunked = {.url = "https://api.github.com/chunked"};
    if (!harness_fetch(&h, &chunked) || h.probe.outcome[0] != MESH_FETCH_OK ||
        strcmp(h.probe.body[0], "hello, world") != 0) {
        failure = "a chunked body should arrive decoded";
        goto cleanup;
    }
    const struct mesh_fetch_request closed = {.url = "https://api.github.com/until-close"};
    if (!harness_fetch(&h, &closed) || h.probe.outcome[1] != MESH_FETCH_OK ||
        strcmp(h.probe.body[1], "all of it") != 0) {
        failure = "a body ended by close_notify should be whole";
        goto cleanup;
    }
    const struct mesh_fetch_request cut = {.url = "https://api.github.com/until-cut"};
    if (!harness_fetch(&h, &cut) || h.probe.outcome[2] != MESH_FETCH_NETWORK) {
        failure = "a body ended by a bare close should be refused";
        goto cleanup;
    }
    const struct mesh_fetch_request shorted = {.url = "https://api.github.com/short"};
    if (!harness_fetch(&h, &shorted) || h.probe.outcome[3] != MESH_FETCH_NETWORK) {
        failure = "a body short of its Content-Length should be refused";
        goto cleanup;
    }

cleanup:
    harness_stop(&h);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * The file a download names appears only for a 2xx, and a range that the server ignored is a
 * failure rather than a file the size of the whole zip.
 */
MESH_TEST_CASE(fetch_writes_a_file_only_for_the_document, unit) {
    struct fetch_harness h;
    const char *failure = NULL;
    char path[64];
    snprintf(path, sizeof path, "/tmp/meshclient-fetch-%d.out", (int)getpid());
    (void)unlink(path);
    if (!harness_start(&h)) {
        failure = "the harness did not start";
        goto cleanup;
    }

    const struct mesh_fetch_request missing = {
        .url = "https://github.com/nothing",
        .output_path = path,
    };
    struct stat info;
    if (!harness_fetch(&h, &missing) || h.probe.outcome[0] != MESH_FETCH_HTTP_STATUS ||
        stat(path, &info) == 0) {
        failure = "a 404 should leave no file behind";
        goto cleanup;
    }

    const struct mesh_fetch_request asset = {
        .url = "https://github.com/release",
        .output_path = path,
    };
    if (!harness_fetch(&h, &asset) || h.probe.outcome[1] != MESH_FETCH_OK ||
        h.probe.body[1][0] != '\0' || stat(path, &info) != 0 || info.st_size != 1000) {
        failure = "the asset should be streamed into the file, not captured";
        goto cleanup;
    }

    const struct mesh_fetch_request whole = {
        .url = "https://github.com/whole",
        .headers = {"Range: bytes=0-3"},
    };
    if (!harness_fetch(&h, &whole) || h.probe.outcome[2] != MESH_FETCH_PROTOCOL ||
        h.probe.status[2] != 200) {
        failure = "a range answered with the whole file should be refused";
        goto cleanup;
    }

cleanup:
    harness_stop(&h);
    (void)unlink(path);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * A reply past its cap is abandoned, and the completion that says so can start the next.
 *
 * Both by name rather than by address, so both go through the resolver: the first request is
 * freed after its completion has run, and freeing it once cancelled the lookup the completion
 * had just started - found on the Brick, where the firmware index's completion starts the
 * manifest's fetch. `127.1` is a name to inet_pton() and an address to getaddrinfo(), which is
 * what puts a lookup in the path without needing DNS.
 */
MESH_TEST_CASE(fetch_caps_a_reply_and_chains_the_next, unit) {
    struct fetch_harness h;
    const char *failure = NULL;
    if (!harness_start(&h)) {
        failure = "the harness did not start";
        goto cleanup;
    }
    mesh_fetch_connect_to(&h.fetch, "127.1", h.server.port);
    h.probe.chain_url = "https://api.github.com/doc";
    const struct mesh_fetch_request big = {
        .url = "https://api.github.com/big",
        .response_max = 1024U,
    };
    if (!harness_fetch(&h, &big) || h.probe.outcome[0] != MESH_FETCH_TOO_LARGE) {
        failure = "a reply past its cap should be abandoned";
        goto cleanup;
    }
    if (!harness_wait(&h, 2U) || h.probe.outcome[1] != MESH_FETCH_OK ||
        strcmp(h.probe.body[1], k_document) != 0) {
        failure = "a request started from inside a completion should run";
        goto cleanup;
    }

cleanup:
    harness_stop(&h);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * The server is checked, always: against the registered roots when nothing overrides them, which
 * the fixture's certificate is not in, and against the URL's own name.
 *
 * The middle leg registers this client's real root set deliberately. Nothing registers roots in
 * the test binary - only `app.c` does - so leaving it unregistered would refuse the certificate
 * for having nothing to check against rather than for not being trusted, and this would pass
 * without testing anything. Those two refusals are worth telling apart, which is what
 * `fetch_refuses_a_session_with_nothing_to_trust` is for.
 */
MESH_TEST_CASE(fetch_verifies_the_server, unit) {
    struct fetch_harness h;
    const char *failure = NULL;
    if (!harness_start(&h)) {
        failure = "the harness did not start";
        goto cleanup;
    }

    const struct mesh_fetch_request stranger = {.url = "https://wrong.example.org/doc"};
    if (!harness_fetch(&h, &stranger) || h.probe.outcome[0] != MESH_FETCH_TLS) {
        failure = "a certificate for other names should be refused";
        goto cleanup;
    }
    unsetenv("SSL_CERT_FILE");
    mesh_tls_set_roots(mesh_ca_roots, mesh_ca_root_count);
    const struct mesh_fetch_request untrusted = {.url = "https://api.github.com/doc"};
    if (!harness_fetch(&h, &untrusted) || h.probe.outcome[1] != MESH_FETCH_TLS) {
        failure = "a certificate outside the registered roots should be refused";
        goto cleanup;
    }
    if (mesh_fetch_start(&h.fetch,
                         &(struct mesh_fetch_request){.url = "http://api.github.com/doc",
                                                      .on_done = probe_record,
                                                      .userdata = &h.probe},
                         0U) != -EINVAL) {
        failure = "a plain http URL should be refused at start";
        goto cleanup;
    }

cleanup:
    mesh_tls_set_roots(NULL, 0U);
    harness_stop(&h);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * The roots the application registered are the ones a connection is actually checked against.
 *
 * Every other HTTPS case in this tree trusts the fixture through `SSL_CERT_FILE`, which is the
 * bundle-file path - a real path, but not the one a shipped build takes. This is the other one:
 * the fixture's own certificate handed over as a trust anchor, with no bundle named, so what is
 * under test is the table `app.c` passes down rather than the override beside it.
 *
 * The second half is the same set replaced. The parse is cached for the life of the process and
 * points into the caller's table without copying, so a registration that did not discard the old
 * parse would keep trusting the fixture here - and, in a build that swapped its roots, keep
 * trusting a root that had been taken away.
 */
MESH_TEST_CASE(fetch_checks_against_the_roots_the_application_registered, unit) {
    struct fetch_harness h;
    const char *failure = NULL;
    mbedtls_x509_crt fixture_crt;
    mbedtls_x509_crt_init(&fixture_crt);

    /* Parsing a certificate is PSA work in 4.x, and this case reads one itself rather than only
       through the TLS client - which would have initialised PSA on the way past. Without this the
       case passes in a whole-suite run, where something earlier has already done it, and fails on
       its own under `--filter`. */
    if (psa_crypto_init() != PSA_SUCCESS) {
        record_failure(test_name, "PSA did not initialise");
        return;
    }

    if (!harness_start(&h)) {
        failure = "the harness did not start";
        goto cleanup;
    }
    /* Out of the bundle path entirely: from here the only trust is what is registered. */
    unsetenv("SSL_CERT_FILE");

    const char *const pem = https_fixture_cert_pem();
    if (mbedtls_x509_crt_parse(&fixture_crt, (const unsigned char *)pem, strlen(pem) + 1U) != 0) {
        failure = "the fixture's certificate did not parse";
        goto cleanup;
    }
    /* `.raw` is the DER the PEM wrapped, which is what a trust anchor is. It belongs to
       `fixture_crt`, so that has to outlive the registration - hence the ordering at cleanup. */
    const struct mesh_tls_ca_root root = {
        .name = "https fixture",
        .der = fixture_crt.raw.p,
        .len = fixture_crt.raw.len,
    };
    mesh_tls_set_roots(&root, 1U);

    const struct mesh_fetch_request trusted = {.url = "https://api.github.com/doc"};
    if (!harness_fetch(&h, &trusted) || h.probe.outcome[0] != MESH_FETCH_OK) {
        failure = "a certificate that is a registered root should verify";
        goto cleanup;
    }

    mesh_tls_set_roots(mesh_ca_roots, mesh_ca_root_count);
    const struct mesh_fetch_request replaced = {.url = "https://api.github.com/doc"};
    if (!harness_fetch(&h, &replaced) || h.probe.outcome[1] != MESH_FETCH_TLS) {
        failure = "replacing the roots should stop the old ones being trusted";
        goto cleanup;
    }

cleanup:
    /* Deregister before the certificate is freed: the cached parse points into it. */
    mesh_tls_set_roots(NULL, 0U);
    mbedtls_x509_crt_free(&fixture_crt);
    harness_stop(&h);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * With nothing registered and no bundle named there is nothing to check against, and that is a
 * refusal rather than a connection.
 *
 * The tempting failure here is the quiet one: no roots read as no verification, and the request
 * succeeds against whoever answered. So the assertion is not only the outcome but that the server
 * logged no request at all - whatever happened, it was not a fetch.
 */
MESH_TEST_CASE(fetch_refuses_a_session_with_nothing_to_trust, unit) {
    struct fetch_harness h;
    const char *failure = NULL;
    if (!harness_start(&h)) {
        failure = "the harness did not start";
        goto cleanup;
    }
    unsetenv("SSL_CERT_FILE");
    mesh_tls_set_roots(NULL, 0U);

    const struct mesh_fetch_request request = {.url = "https://api.github.com/doc"};
    if (!harness_fetch(&h, &request) || h.probe.outcome[0] != MESH_FETCH_TLS) {
        failure = "a session with no trust anchors should fail, not connect";
        goto cleanup;
    }
    char log[256];
    const size_t logged = https_fixture_requests(&h.server, log, sizeof log);
    if (logged != 0U) {
        failure = "no request should have reached the server";
        goto cleanup;
    }

cleanup:
    harness_stop(&h);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * A host's addresses are tried in turn, so one that will not take a connection is not the end of
 * the request. The first is refused outright - 127.0.0.2 is loopback with nothing listening - and
 * the second is TEST-NET-1, which never answers at all and is given up on by the clock; the third
 * is the server. A dual-stack network with no IPv6 route is the case this is for: every AAAA
 * comes first and every one of them is the second kind.
 */
MESH_TEST_CASE(fetch_tries_the_next_address, unit) {
    struct fetch_harness h;
    const char *failure = NULL;
    if (!harness_start(&h)) {
        failure = "the harness did not start";
        goto cleanup;
    }
    mesh_fetch_connect_to(&h.fetch, "127.0.0.2,192.0.2.1,127.0.0.1", h.server.port);
    const struct mesh_fetch_request doc = {
        .url = "https://api.github.com/doc",
        .timeout_ms = 60000U,
        .on_done = probe_record,
        .userdata = &h.probe,
    };
    if (mesh_fetch_start(&h.fetch, &doc, 0U) != 0) {
        failure = "the request should start";
        goto cleanup;
    }
    /* Past each address's allowance in turn, and well inside the request's own. */
    uint64_t now = 0U;
    for (int turn = 0; turn < 600 && h.probe.calls == 0U; ++turn) {
        (void)inkwell_loop_run(&h.loop, 10);
        if (turn % 20 == 19) {
            now += 4000U;
        }
        mesh_fetch_tick(&h.fetch, now);
    }
    if (h.probe.calls != 1U || h.probe.outcome[0] != MESH_FETCH_OK ||
        strcmp(h.probe.body[0], k_document) != 0) {
        failure = "the address that answers should be reached past the two that do not";
        goto cleanup;
    }
    if (h.fetch.preferred_family != AF_INET) {
        failure = "the family that connected should be remembered";
        goto cleanup;
    }

cleanup:
    harness_stop(&h);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

MESH_TEST_CASE(fetch_gives_up_on_a_server_that_does_not_answer, unit) {
    struct fetch_harness h;
    const char *failure = NULL;
    if (!harness_start(&h)) {
        failure = "the harness did not start";
        goto cleanup;
    }

    const struct mesh_fetch_request slow = {
        .url = "https://api.github.com/slow",
        .timeout_ms = 1000U,
        .on_done = probe_record,
        .userdata = &h.probe,
    };
    if (mesh_fetch_start(&h.fetch, &slow, 0U) != 0) {
        failure = "the request should start";
        goto cleanup;
    }
    for (int turn = 0; turn < 20; ++turn) {
        (void)inkwell_loop_run(&h.loop, 10);
        mesh_fetch_tick(&h.fetch, 0U);
    }
    if (h.probe.calls != 0U) {
        failure = "nothing should have finished before the deadline";
        goto cleanup;
    }
    mesh_fetch_tick(&h.fetch, 1000U);
    if (h.probe.calls != 1U || h.probe.outcome[0] != MESH_FETCH_TIMED_OUT ||
        mesh_fetch_busy(&h.fetch)) {
        failure = "the deadline should end the request";
        goto cleanup;
    }

    /* And nothing listening is a network failure, not a hang. */
    mesh_fetch_connect_to(&h.fetch, "127.0.0.1", 1U);
    const struct mesh_fetch_request refused = {.url = "https://api.github.com/doc"};
    if (!harness_fetch(&h, &refused) || h.probe.outcome[1] != MESH_FETCH_NETWORK) {
        failure = "a refused connection should be reported as the network";
        goto cleanup;
    }

cleanup:
    harness_stop(&h);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

#endif /* MESHCLIENT_HAVE_TLS */
