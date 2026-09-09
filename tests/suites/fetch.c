#define _POSIX_C_SOURCE 200809L

/*
 * The forked fetcher: one child, one outcome, and the one thing about it that is not obvious -
 * that a caller may start its next request from inside the completion of the last.
 *
 * Every case here puts a shell script called `curl` on PATH, which is what the updater suite
 * next door does and for the same reason: the shape being tested is fork, read, reap, and a
 * script can be told to exit 22 or to print more than a cap allows on demand.
 */

#include "framework/mesh_test.h"

#include "mesh/core/event_loop.h"
#include "mesh/core/fetch.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct fetch_probe {
    struct mesh_fetch *fetch;
    /* What each completion saw, in the order they arrived. */
    enum mesh_fetch_outcome outcome[4];
    int status[4];
    char body[4][64];
    unsigned calls;
    /* When set, the first completion starts this URL again - the chained case. */
    const char *chain_url;
};

static void probe_record(void *userdata, const struct mesh_fetch_result *result);

static void probe_record(void *userdata, const struct mesh_fetch_result *result) {
    struct fetch_probe *probe = (struct fetch_probe *)userdata;
    if (probe->calls < sizeof probe->outcome / sizeof probe->outcome[0]) {
        const unsigned slot = probe->calls;
        probe->outcome[slot] = result->outcome;
        probe->status[slot] = result->status;
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

/* Pumps the loop until the probe has seen `wanted` completions, or the budget runs out. */
static bool fetch_wait_for(struct mesh_event_loop *loop, struct mesh_fetch *fetch,
                           const struct fetch_probe *probe, unsigned wanted) {
    for (int turn = 0; turn < 400 && probe->calls < wanted; ++turn) {
        (void)mesh_event_loop_run(loop, 10);
        mesh_fetch_tick(fetch, 0U);
    }
    return probe->calls >= wanted;
}

/* Writes an executable `curl` in `dir` whose body is `script`, and puts `dir` on PATH. The
   caller frees the returned copy of the old PATH and restores it. */
static char *fetch_install_fake_curl(const char *dir, const char *script) {
    char path[256];
    snprintf(path, sizeof path, "%s/curl", dir);
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return NULL;
    }
    fputs(script, file);
    fclose(file);
    if (chmod(path, 0755) != 0) {
        return NULL;
    }
    const char *const old_path = getenv("PATH");
    char *const saved = old_path != NULL ? strdup(old_path) : strdup("");
    char new_path[1024];
    snprintf(new_path, sizeof new_path, "%s:%s", dir, old_path != NULL ? old_path : "/usr/bin");
    setenv("PATH", new_path, 1);
    return saved;
}

MESH_TEST_CASE(fetch_reports_body_and_exit, unit) {
    char dir[] = "/tmp/meshclient_fetch_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");

    /* Echoes its last argument, so the "document" is whatever URL was asked for - and exits
       with the status the URL names when it names one. */
    char *saved_path = fetch_install_fake_curl(dir,
                                               "#!/bin/sh\n"
                                               "for a in \"$@\"; do url=\"$a\"; done\n"
                                               "case \"$url\" in\n"
                                               "  *fail) exit 22 ;;\n"
                                               "esac\n"
                                               "printf 'document:%s' \"$url\"\n");
    const char *failure = NULL;
    struct mesh_event_loop loop;
    struct mesh_fetch fetch;
    bool loop_up = false;
    bool fetch_up = false;
    if (saved_path == NULL) {
        failure = "could not install the fake curl";
        goto cleanup;
    }
    if (mesh_event_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (mesh_fetch_init(&fetch, &loop) != 0) {
        failure = "fetch init failed";
        goto cleanup;
    }
    fetch_up = true;
    if (fetch.tool == NULL || strcmp(fetch.tool, "curl") != 0) {
        failure = "the fake curl should have been picked up from PATH";
        goto cleanup;
    }

    struct fetch_probe probe;
    memset(&probe, 0, sizeof probe);
    probe.fetch = &fetch;

    const struct mesh_fetch_request ok = {
        .url = "https://example.invalid/ok",
        .timeout_ms = 5000U,
        .on_done = probe_record,
        .userdata = &probe,
    };
    if (mesh_fetch_start(&fetch, &ok, 0U) != 0) {
        failure = "a first request should start";
        goto cleanup;
    }
    if (mesh_fetch_start(&fetch, &ok, 0U) != -EBUSY) {
        failure = "a second request while one is in flight should be refused";
        goto cleanup;
    }
    if (!fetch_wait_for(&loop, &fetch, &probe, 1U)) {
        failure = "the request should have completed";
        goto cleanup;
    }
    if (probe.outcome[0] != MESH_FETCH_OK ||
        strcmp(probe.body[0], "document:https://example.invalid/ok") != 0) {
        failure = "the captured body should be what the child printed";
        goto cleanup;
    }
    if (mesh_fetch_busy(&fetch)) {
        failure = "the fetcher should be idle once its completion has run";
        goto cleanup;
    }

    const struct mesh_fetch_request bad = {
        .url = "https://example.invalid/fail",
        .timeout_ms = 5000U,
        .on_done = probe_record,
        .userdata = &probe,
    };
    if (mesh_fetch_start(&fetch, &bad, 0U) != 0) {
        failure = "a request after a completed one should start";
        goto cleanup;
    }
    if (!fetch_wait_for(&loop, &fetch, &probe, 2U)) {
        failure = "the failing request should have completed";
        goto cleanup;
    }
    if (probe.outcome[1] != MESH_FETCH_EXITED || probe.status[1] != 22) {
        failure = "a non-zero exit should be reported with its status";
        goto cleanup;
    }

cleanup:
    if (fetch_up) {
        mesh_fetch_shutdown(&fetch);
    }
    if (loop_up) {
        mesh_event_loop_shutdown(&loop);
    }
    if (saved_path != NULL) {
        setenv("PATH", saved_path, 1);
        free(saved_path);
    }
    char curl_path[256];
    snprintf(curl_path, sizeof curl_path, "%s/curl", dir);
    unlink(curl_path);
    rmdir(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A reply larger than the request's cap, and the next request started from inside the
 * completion that reported it.
 *
 * The chaining is the case worth pinning: the radio-firmware check reads two documents in
 * sequence and asks for the second the moment the first arrives, so a fetcher that was still
 * holding the last child's buffer when the next start() ran would either refuse it or free the
 * wrong allocation. Doing it on the *failing* completion is deliberate - that is the path where
 * the fetcher tore its own child down rather than reaping a clean exit.
 */
MESH_TEST_CASE(fetch_caps_the_reply_and_chains, unit) {
    char dir[] = "/tmp/meshclient_fetch_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");

    char *saved_path = fetch_install_fake_curl(dir,
                                               "#!/bin/sh\n"
                                               "for a in \"$@\"; do url=\"$a\"; done\n"
                                               "case \"$url\" in\n"
                                               "  *big) exec head -c 20000 /dev/zero ;;\n"
                                               "esac\n"
                                               "printf 'second'\n");
    const char *failure = NULL;
    struct mesh_event_loop loop;
    struct mesh_fetch fetch;
    bool loop_up = false;
    bool fetch_up = false;
    if (saved_path == NULL) {
        failure = "could not install the fake curl";
        goto cleanup;
    }
    if (mesh_event_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (mesh_fetch_init(&fetch, &loop) != 0) {
        failure = "fetch init failed";
        goto cleanup;
    }
    fetch_up = true;

    struct fetch_probe probe;
    memset(&probe, 0, sizeof probe);
    probe.fetch = &fetch;
    probe.chain_url = "https://example.invalid/next";

    const struct mesh_fetch_request big = {
        .url = "https://example.invalid/big",
        .timeout_ms = 5000U,
        .response_max = 4096U,
        .on_done = probe_record,
        .userdata = &probe,
    };
    if (mesh_fetch_start(&fetch, &big, 0U) != 0) {
        failure = "the oversized request should start";
        goto cleanup;
    }
    if (!fetch_wait_for(&loop, &fetch, &probe, 2U)) {
        failure = "both the capped request and the one chained off it should complete";
        goto cleanup;
    }
    if (probe.outcome[0] != MESH_FETCH_TOO_LARGE) {
        failure = "a reply past the cap should be reported as too large";
        goto cleanup;
    }
    if (probe.outcome[1] != MESH_FETCH_OK || strcmp(probe.body[1], "second") != 0) {
        failure = "a request started from inside a completion should run and be captured";
        goto cleanup;
    }

cleanup:
    if (fetch_up) {
        mesh_fetch_shutdown(&fetch);
    }
    if (loop_up) {
        mesh_event_loop_shutdown(&loop);
    }
    if (saved_path != NULL) {
        setenv("PATH", saved_path, 1);
        free(saved_path);
    }
    char curl_path[256];
    snprintf(curl_path, sizeof curl_path, "%s/curl", dir);
    unlink(curl_path);
    rmdir(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
