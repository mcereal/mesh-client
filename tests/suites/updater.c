#define _POSIX_C_SOURCE 200809L

/* Version comparison and the self-update lifecycle. */

#include "framework/mesh_test.h"

#include "inkwell/base/version.h"
#include "inkwell/codec/sha256.h"
#include "inkwell/runtime/loop.h"
#include "mesh/core/updater.h"
#include "mesh/core/version.h"
#include "mesh/i18n/strings.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* A release payload shaped like the one api.github.com actually returns, trimmed to the keys
   the updater reads plus enough noise to catch a scanner that latches onto the wrong one. */
static const char k_release_json[] =
    "{\"tag_name\":\"v1.13.0\",\"name\":\"v1.13.0\",\"draft\":false,\"prerelease\":false,"
    "\"body\":\"### Features\\n* something with \\\"name\\\": \\\"decoy\\\" inside it\","
    "\"assets\":["
    "{\"name\":\"MeshClient.pak.zip\",\"size\":949158,"
    "\"browser_download_url\":\"https://github.com/mcereal/mesh-client/releases/download/"
    "v1.13.0/MeshClient.pak.zip\",\"digest\":\"sha256:"
    "1111111111111111111111111111111111111111111111111111111111111111\"},"
    "{\"name\":\"meshclient-tg5040-aarch64\",\"size\":874112,"
    "\"browser_download_url\":\"https://github.com/mcereal/mesh-client/releases/download/"
    "v1.13.0/meshclient-tg5040-aarch64\",\"digest\":\"sha256:"
    "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\"}"
    "]}";

/* Runs the loop until `updater` leaves `from`, or the budget runs out. Returns true if it
   moved: every step arrives through the event loop, so the test has to pump it. */
static bool updater_wait_past(struct inkwell_loop *loop, struct mesh_updater *updater,
                              enum mesh_update_state from) {
    for (int i = 0; i < 200 && updater->state == from; ++i) {
        inkwell_loop_run(loop, 50);
        mesh_updater_tick(updater, (uint64_t)i * 50U);
    }
    return updater->state != from;
}

/* ---- client version and self-update ------------------------------------------------------- */

/*
 * What this build says it is, and what it will therefore accept as an update.
 *
 * The ordering itself is not here any more - it is arithmetic over two strings and it lives in
 * inkwell's base_version suite, which has the whole table of pairs. What is left is the half
 * only this build can answer: that a version was baked in at all, and that
 * mesh_version_is_newer_than_running() binds that version to the comparison rather than
 * answering about some other pair of strings.
 */
MESH_TEST_CASE(version_compare, unit) {
    /* The build under test always reports something, release-stamped or not. Whether it is
     *offered* an update is a separate question, covered by version_build_stamp. */
    MESH_TEST_FAIL_IF(mesh_version_string()[0] == '\0',
                      "the test build should carry a baked-in version");
    MESH_TEST_FAIL_IF(mesh_version_is_newer_than_running(mesh_version_string()) ||
                          mesh_version_is_newer_than_running("0.0.1") ||
                          mesh_version_is_newer_than_running("not-a-version"),
                      "nothing at or below the running version is an update");
    record_success(test_name);
}

MESH_TEST_CASE(updater_parse_release, unit) {
    const char *repo = "mcereal/mesh-client";
    const char *asset = "meshclient-tg5040-aarch64";

    char tag[MESH_UPDATE_VERSION_MAX];
    char url[MESH_UPDATE_URL_MAX];
    char sha[65];
    uint64_t size = 0U;

    MESH_TEST_FAIL_IF(!mesh_updater_parse_release(k_release_json, repo, asset, tag, sizeof tag, url,
                                                  sizeof url, sha, sizeof sha, &size),
                      "a well-formed release should parse");
    /* The tag loses its leading v so it can be compared against the baked-in version. */
    MESH_TEST_FAIL_IF(strcmp(tag, "1.13.0") != 0, tag);
    /* The second asset's URL, not the first one's: the scanner must follow the matched name. */
    MESH_TEST_FAIL_IF(strcmp(url,
                             "https://github.com/mcereal/mesh-client/releases/download/v1.13.0/"
                             "meshclient-tg5040-aarch64") != 0,
                      url);
    MESH_TEST_FAIL_IF(
        strcmp(sha, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789") != 0, sha);
    MESH_TEST_FAIL_IF(size != 874112U, "the asset size should come from the matched asset");

    /* An asset the release does not carry is not an error to paper over. */
    MESH_TEST_FAIL_IF(mesh_updater_parse_release(k_release_json, repo, "meshclient-nonesuch", tag,
                                                 sizeof tag, url, sizeof url, sha, sizeof sha,
                                                 &size),
                      "a missing asset should not parse");

    /*
     * The security-relevant case: the download URL is only accepted when it is under this
     * repository's release-download path. A response that points somewhere else is refused
     * outright rather than downloaded and hashed, because the digest beside it would just be
     * the attacker's digest.
     */
    static const char *const k_bad_urls[] = {
        "http://github.com/mcereal/mesh-client/releases/download/v1.13.0/meshclient",   /* no TLS */
        "https://github.com.evil.test/mcereal/mesh-client/releases/download/v1/mesh",   /* host */
        "https://github.com/someone/else/releases/download/v1.13.0/meshclient",         /* repo */
        "https://github.com/mcereal/mesh-client/releases/download/../../../etc/passwd", /* climb */
        "https://github.com/mcereal/mesh-client/releases/download/",                    /* empty */
    };
    for (size_t i = 0; i < sizeof k_bad_urls / sizeof k_bad_urls[0]; ++i) {
        char json[1024];
        snprintf(json, sizeof json,
                 "{\"tag_name\":\"v9.9.9\",\"assets\":[{\"name\":\"%s\",\"size\":10,"
                 "\"browser_download_url\":\"%s\",\"digest\":\"sha256:%064d\"}]}",
                 asset, k_bad_urls[i], 0);
        MESH_TEST_FAIL_IF(mesh_updater_parse_release(json, repo, asset, tag, sizeof tag, url,
                                                     sizeof url, sha, sizeof sha, &size),
                          k_bad_urls[i]);
    }

    /* A malformed digest is dropped rather than carried through; the updater refuses to
       install without one, so this is what keeps an unverifiable release from being offered. */
    char json[1024];
    snprintf(json, sizeof json,
             "{\"tag_name\":\"v9.9.9\",\"assets\":[{\"name\":\"%s\",\"size\":10,"
             "\"browser_download_url\":\"https://github.com/%s/releases/download/v9.9.9/%s\","
             "\"digest\":\"md5:deadbeef\"}]}",
             asset, repo, asset);
    MESH_TEST_FAIL_IF(!mesh_updater_parse_release(json, repo, asset, tag, sizeof tag, url,
                                                  sizeof url, sha, sizeof sha, &size) ||
                          sha[0] != '\0',
                      "a non-sha256 digest should be dropped");

    /* Truncated and empty payloads must fail rather than read past the end. */
    static const char *const k_broken[] = {
        "", "{", "{\"tag_name\":", "{\"tag_name\":\"v1.0.0\"}", "not json at all",
    };
    for (size_t i = 0; i < sizeof k_broken / sizeof k_broken[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_updater_parse_release(k_broken[i], repo, asset, tag, sizeof tag, url,
                                                     sizeof url, sha, sizeof sha, &size),
                          "a broken payload should not parse");
    }
    record_success(test_name);
}

MESH_TEST_CASE(updater_lifecycle, unit) {
    struct inkwell_loop loop;
    MESH_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "event loop init failed");

    struct mesh_updater updater;
    if (mesh_updater_init(&updater, &loop) != 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "updater init failed");
        return;
    }
    if (updater.state != MESH_UPDATE_IDLE || updater.revision != 0U) {
        mesh_updater_shutdown(&updater);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "a fresh updater should be idle");
        return;
    }
    /* init asks the system for the running binary, so the staged name must sit beside it - the
       rename that installs it is only atomic within one directory. On macOS the target is the
       .app around the binary, and a test binary is in none: see updater_find_install_target(). */
#if defined(__linux__) || defined(_WIN32)
    const bool placed =
        updater.install_path[0] != '\0' &&
        strncmp(updater.staged_path, updater.install_path, strlen(updater.install_path)) == 0;
#else
    const bool placed = updater.install_path[0] == '\0';
#endif
    if (!placed) {
        mesh_updater_shutdown(&updater);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "the staged path should sit next to the installed one");
        return;
    }

    /* Install is only reachable from AVAILABLE with an asset in hand; from IDLE it is a
       programming error, not a no-op that silently downloads nothing. */
    if (mesh_updater_install(&updater, 0U) == 0) {
        mesh_updater_shutdown(&updater);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "install from idle should be refused");
        return;
    }

    /* An updater with no event loop reports itself unavailable rather than half-working. */
    struct mesh_updater detached;
    mesh_updater_init(&detached, NULL);
    if (mesh_updater_available(&detached) || mesh_updater_check(&detached, 0U) != -ENOTSUP) {
        mesh_updater_shutdown(&detached);
        mesh_updater_shutdown(&updater);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "an updater with no loop should be unavailable");
        return;
    }
    mesh_updater_shutdown(&detached);

    /* tick() on an idle updater must not touch a child it does not have. */
    mesh_updater_tick(&updater, 1000000U);
    if (updater.state != MESH_UPDATE_IDLE) {
        mesh_updater_shutdown(&updater);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "ticking an idle updater should change nothing");
        return;
    }

    if (mesh_update_state_name(MESH_UPDATE_READY) == NULL ||
        strcmp(mesh_update_state_name(MESH_UPDATE_IDLE), "idle") != 0) {
        mesh_updater_shutdown(&updater);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "every state should have a name");
        return;
    }

    /* A fresh updater is on DEFAULT, which resolves to one of the two real channels - never
       back to DEFAULT, or check() would have no endpoint to pick. */
    const char *channel_failure = NULL;
    if (updater.channel != MESH_UPDATE_CHANNEL_DEFAULT) {
        channel_failure = "a fresh updater should be on the default channel";
    } else if (mesh_updater_effective_channel(&updater) == MESH_UPDATE_CHANNEL_DEFAULT) {
        channel_failure = "the default channel should resolve to a real one";
    } else if ((strstr(mesh_update_channel_name(MESH_UPDATE_CHANNEL_DEFAULT), "prerelease") !=
                NULL) !=
               (mesh_updater_effective_channel(&updater) == MESH_UPDATE_CHANNEL_PRERELEASE)) {
        /* The label has to name the endpoint the check will actually use. It did not: a `-dev`
           suffix makes mesh_version_is_prerelease() true by itself, so every local build read
           "Automatic (prerelease)" while querying the stable endpoint. */
        channel_failure = "the default channel's label should name the channel it resolves to";
    } else if (mesh_updater_set_channel(&updater, MESH_UPDATE_CHANNEL_DEFAULT)) {
        channel_failure = "setting the channel it already has should be a no-op";
    } else if (!mesh_updater_set_channel(&updater, MESH_UPDATE_CHANNEL_PRERELEASE) ||
               mesh_updater_effective_channel(&updater) != MESH_UPDATE_CHANNEL_PRERELEASE) {
        channel_failure = "the channel should be settable";
    }

    /*
     * Switching channel must drop whatever the last check found. The release held here belongs
     * to the question that was asked, and installing a prerelease asset after switching back
     * to stable is exactly the mismatch this guards.
     */
    if (channel_failure == NULL) {
        updater.state = MESH_UPDATE_AVAILABLE;
        snprintf(updater.latest, sizeof updater.latest, "%s", "9.9.9");
        snprintf(updater.asset_url, sizeof updater.asset_url, "%s",
                 "https://github.com/x/y/releases/download/v9.9.9/asset");
        memset(updater.asset_sha256, 'a', 64);
        updater.asset_sha256[64] = '\0';
        updater.asset_size = 1024U;
        if (!mesh_updater_set_channel(&updater, MESH_UPDATE_CHANNEL_STABLE)) {
            channel_failure = "switching channel should take";
        } else if (updater.state != MESH_UPDATE_IDLE || updater.latest[0] != '\0' ||
                   updater.asset_url[0] != '\0' || updater.asset_sha256[0] != '\0' ||
                   updater.asset_size != 0U) {
            channel_failure = "switching channel should forget the release the last check found";
        } else if (mesh_updater_install(&updater, 0U) == 0) {
            channel_failure = "install after a channel switch should be refused";
        }
    }
    if (channel_failure != NULL) {
        mesh_updater_shutdown(&updater);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, channel_failure);
        return;
    }

    /*
     * The dev-updates opt-in. The tests are not a release build, so can_install tracks it
     * exactly - and flipping it has to invalidate the last check for the same reason a channel
     * change does: "not installing" and "available" are different answers to one question.
     */
    const char *dev_failure = NULL;
    if (!updater.allow_dev_from_env) {
        if (mesh_updater_can_install(&updater)) {
            dev_failure = "a dev build should not install by default";
        } else if (!mesh_updater_set_allow_dev(&updater, true) ||
                   !mesh_updater_can_install(&updater)) {
            dev_failure = "the dev-updates opt-in should take";
        } else if (mesh_updater_set_allow_dev(&updater, true)) {
            dev_failure = "setting the opt-in it already has should be a no-op";
        } else {
            updater.state = MESH_UPDATE_AVAILABLE;
            snprintf(updater.latest, sizeof updater.latest, "%s", "9.9.9");
            snprintf(updater.asset_url, sizeof updater.asset_url, "%s",
                     "https://github.com/x/y/releases/download/v9.9.9/asset");
            if (!mesh_updater_set_allow_dev(&updater, false)) {
                dev_failure = "turning the opt-in off should take";
            } else if (updater.state != MESH_UPDATE_IDLE || updater.latest[0] != '\0' ||
                       updater.asset_url[0] != '\0') {
                dev_failure = "turning the opt-in off should forget what the last check found";
            } else if (mesh_updater_can_install(&updater)) {
                dev_failure = "a dev build should not install once the opt-in is off again";
            }
        }
    }
    if (dev_failure != NULL) {
        mesh_updater_shutdown(&updater);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, dev_failure);
        return;
    }

    mesh_updater_shutdown(&updater);
    inkwell_loop_shutdown(&loop);
    record_success(test_name);
}

#ifdef INKWELL_HAVE_TLS

#include "support/https_fixture.h"

/* What the fake GitHub serves, named by path so a case can rewrite a file between requests. */
struct updater_github {
    char json[256];
    char payload[256];
    char gate[256];
    size_t half;
};

/*
 * GitHub, in the fixture's child: the API answers with the release document, the release asset
 * URL is a 302 to another host the way github.com's really is, and that host serves the binary.
 *
 * The binary arrives in two pieces with the test holding a gate between them, which is what
 * makes the progress meter testable rather than raced. Byte progress is read by stat()ing the
 * file the fetcher is writing (see `downloaded` in updater.h), so a transfer that stops half way
 * is precisely a download the UI has to be able to report a fraction of - and a fixed first
 * piece makes that fraction an exact number rather than whatever the scheduler allowed.
 */
static void updater_serve(void *userdata, const struct https_fixture_request *request,
                          struct https_fixture_conn *conn) {
    const struct updater_github *const github = (const struct updater_github *)userdata;
    if (strcmp(request->host, "api.github.com") == 0) {
        https_fixture_reply_file(conn, request, github->json);
        return;
    }
    if (strcmp(request->host, "github.com") == 0) {
        https_fixture_reply(
            conn, 302, "Location: https://objects.githubusercontent.com/asset?sig=x\r\n", NULL, 0U);
        return;
    }
    if (strcmp(request->host, "objects.githubusercontent.com") != 0) {
        https_fixture_reply(conn, 404, NULL, NULL, 0U);
        return;
    }
    char payload[256];
    FILE *file = fopen(github->payload, "rb");
    const size_t len = file != NULL ? fread(payload, 1U, sizeof payload, file) : 0U;
    if (file != NULL) {
        fclose(file);
    }
    https_fixture_printf(conn, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", len);
    https_fixture_send(conn, payload, github->half);
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 20000000L};
    while (access(github->gate, F_OK) != 0) {
        (void)nanosleep(&pause, NULL);
    }
    https_fixture_send(conn, payload + github->half, len - github->half);
}

#if defined(__APPLE__)
/*
 * Turns the payload into what a Mac release serves - `ditto`'s zip of a MeshClient.app whose
 * executable is the payload - and puts an older bundle at `install_path` for it to replace.
 * A Mac installs the whole bundle rather than the executable (see updater_install_bundle()), so
 * this is what exercises that path for real, `ditto` and all.
 */
static bool updater_make_bundle_zip(const char *dir, const char *payload_path,
                                    const char *install_path) {
    char command[1536];
    const int written =
        snprintf(command, sizeof command,
                 "set -e; mkdir -p '%s/new/MeshClient.app/Contents/MacOS' '%s/Contents/MacOS'; "
                 "cp '%s' '%s/new/MeshClient.app/Contents/MacOS/meshclient'; "
                 "chmod 755 '%s/new/MeshClient.app/Contents/MacOS/meshclient'; "
                 "printf old > '%s/Contents/MacOS/meshclient'; "
                 "ditto -c -k --keepParent '%s/new/MeshClient.app' '%s'",
                 dir, install_path, payload_path, dir, dir, install_path, dir, payload_path);
    return written > 0 && (size_t)written < sizeof command && system(command) == 0;
}
#endif

/*
 * The whole update path against a real HTTPS server: the check, the redirect to the asset's
 * host, the download streamed to disk, the checksum and the rename into place.
 *
 * Worth doing for real rather than mocking the pieces, because the bugs this path attracts are
 * in the seams - a reply read short, a completion that lands while the loop is somewhere else -
 * and none of those show up when the fetch is stubbed out.
 */
MESH_TEST_CASE(updater_fetch_and_install, unit) {
    char dir[] = "/tmp/meshclient_update_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");
    const char *failure = NULL;
    struct https_fixture server;
    memset(&server, 0, sizeof server);
    struct inkwell_loop loop;
    struct mesh_updater updater;
    bool loop_up = false;
    bool updater_up = false;

    char payload_path[256];
    char json_path[256];
    char install_path[256];
    char bin_dir[256];
    char shared_dir[256];
    char pak_json_path[256];
    snprintf(payload_path, sizeof payload_path, "%s/payload", dir);
    snprintf(json_path, sizeof json_path, "%s/release.json", dir);
    /* The pak layout, because the install stamps the pak.json two directories above the
       binary and would find nothing in a flat one. */
    snprintf(bin_dir, sizeof bin_dir, "%s/bin", dir);
    snprintf(shared_dir, sizeof shared_dir, "%s/bin/shared", dir);
    snprintf(install_path, sizeof install_path, "%s/bin/shared/meshclient", dir);
    snprintf(pak_json_path, sizeof pak_json_path, "%s/pak.json", dir);
    /* The executable the install leaves behind: the target itself, or on a Mac the one inside
       the bundle that is the target. */
    char installed_binary[320];
#if defined(__APPLE__)
    snprintf(install_path, sizeof install_path, "%s/MeshClient.app", dir);
    snprintf(installed_binary, sizeof installed_binary, "%s/Contents/MacOS/meshclient",
             install_path);
#else
    snprintf(installed_binary, sizeof installed_binary, "%s", install_path);
#endif
    /* The gate the server waits on before finishing its download - see updater_serve(). */
    char gate_path[256];
    snprintf(gate_path, sizeof gate_path, "%s/finish-download", dir);
    MESH_TEST_FAIL_IF(mkdir(bin_dir, 0755) != 0 || mkdir(shared_dir, 0755) != 0,
                      "could not create the pak layout");
    FILE *pak_json = fopen(pak_json_path, "wb");
    MESH_TEST_FAIL_IF(pak_json == NULL, "could not write pak.json");
    fprintf(pak_json, "{\n  \"name\": \"MeshClient\",\n  \"version\": \"v1.0.0\",\n"
                      "  \"type\": \"TOOL\"\n}\n");
    fclose(pak_json);

    /* The "new binary", and the digest the release will claim for it. */
    static const char k_payload[] = "#!/bin/sh\nexit 0\n";
    FILE *payload = fopen(payload_path, "wb");
    if (payload == NULL ||
        fwrite(k_payload, 1U, sizeof k_payload - 1U, payload) != sizeof k_payload - 1U) {
        if (payload != NULL) {
            fclose(payload);
        }
        failure = "could not write the payload";
        goto cleanup;
    }
    fclose(payload);

    /* What the installed executable must hash to, and then what the release serves - the same
       file, except on a Mac, where the release serves a zip of a bundle around it. */
    uint8_t binary_digest[INKWELL_SHA256_DIGEST_LEN];
    if (inkwell_sha256_file(payload_path, binary_digest) != 0) {
        failure = "could not hash the payload";
        goto cleanup;
    }
#if defined(__APPLE__)
    if (!updater_make_bundle_zip(dir, payload_path, install_path)) {
        failure = "could not zip a bundle with ditto";
        goto cleanup;
    }
#endif
    uint8_t digest[INKWELL_SHA256_DIGEST_LEN];
    char digest_hex[INKWELL_SHA256_HEX_LEN];
    struct stat served;
    if (inkwell_sha256_file(payload_path, digest) != 0 || stat(payload_path, &served) != 0) {
        failure = "could not hash the payload";
        goto cleanup;
    }
    inkwell_sha256_hex(digest, digest_hex, sizeof digest_hex);
    const size_t payload_size = (size_t)served.st_size;
    /* What the fetcher writes before it stops, so the fraction the meter reads is an exact
       number rather than however far the scheduler got. */
    const size_t k_payload_half = payload_size / 2U;

    FILE *json = fopen(json_path, "wb");
    if (json == NULL) {
        failure = "could not write the release json";
        goto cleanup;
    }
    fprintf(json,
            "{\"tag_name\":\"v999.0.0\",\"assets\":[{\"name\":\"meshclient-tg5040-aarch64\","
            "\"size\":%zu,\"browser_download_url\":\"https://github.com/mcereal/mesh-client/"
            "releases/download/v999.0.0/meshclient-tg5040-aarch64\",\"digest\":\"sha256:%s\"}]}",
            payload_size, digest_hex);
    fclose(json);

    static struct updater_github github;
    snprintf(github.json, sizeof github.json, "%s", json_path);
    snprintf(github.payload, sizeof github.payload, "%s", payload_path);
    snprintf(github.gate, sizeof github.gate, "%s", gate_path);
    github.half = k_payload_half;
    if (!https_fixture_start(&server, updater_serve, &github)) {
        failure = "could not stand up the fake GitHub";
        goto cleanup;
    }

    if (inkwell_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (mesh_updater_init(&updater, &loop) != 0) {
        failure = "updater init failed";
        goto cleanup;
    }
    updater_up = true;
    https_fixture_attach(&server, &updater.fetch);

    /* Never let the install rename over the running test binary. */
    snprintf(updater.install_path, sizeof updater.install_path, "%s", install_path);
    snprintf(updater.staged_path, sizeof updater.staged_path, "%s.update", install_path);

    if (mesh_updater_check(&updater, 0U) != 0 || updater.state != MESH_UPDATE_CHECKING) {
        failure = "check should start";
        goto cleanup;
    }
    if (!updater_wait_past(&loop, &updater, MESH_UPDATE_CHECKING)) {
        failure = "the check never finished";
        goto cleanup;
    }
    /* This binary is not release-stamped, so even a 999.0.0 release is deliberately not
       offered - that is the safeguard that stops a locally built client being replaced. */
    if (updater.state != MESH_UPDATE_UP_TO_DATE) {
        failure = updater.message[0] != '\0' ? updater.message
                                             : "an unstamped build should not be offered 999.0.0";
        goto cleanup;
    }
    if (strcmp(updater.latest, "999.0.0") != 0 || strcmp(updater.asset_sha256, digest_hex) != 0 ||
        updater.asset_size != payload_size) {
        failure = "the release metadata should have been drained and parsed in full";
        goto cleanup;
    }

    /* The metadata is all there, so drive the install path from it directly. A release build
       reaches this state through the check; here it is set so the download, the checksum and
       the swap are still exercised without pretending this binary is a release. */
    updater.state = MESH_UPDATE_AVAILABLE;

    if (mesh_updater_install(&updater, 0U) != 0 || updater.state != MESH_UPDATE_DOWNLOADING) {
        failure = "install should start";
        goto cleanup;
    }

    /*
     * The progress meter's data, read off the file the download is going into.
     *
     * Nothing has been written yet, and a download with a size to divide by is a *known* zero
     * rather than an unknown - the distinction the About screen draws as an empty bar rather
     * than as a moving one.
     */
    uint32_t permille = 999U;
    if (!mesh_updater_progress(&updater, &permille) || permille != 0U) {
        failure = "a download that has written nothing should report a known 0";
        goto cleanup;
    }

    /* The server has sent its first piece and is waiting on the gate, so the reading is
       exactly that piece over the asset's size and stays there until the test lets go. */
    for (int i = 0; i < 200 && updater.downloaded == 0U; ++i) {
        inkwell_loop_run(&loop, 10);
        mesh_updater_tick(&updater, (uint64_t)i * 10U);
    }
    if (updater.downloaded != k_payload_half) {
        failure = "a half-written download should be read off the staged file";
        goto cleanup;
    }
    const uint32_t expect = (uint32_t)(k_payload_half * 1000U / payload_size);
    if (!mesh_updater_progress(&updater, &permille) || permille != expect) {
        failure = "a half-written download should report its own fraction";
        goto cleanup;
    }

    FILE *gate = fopen(gate_path, "wb");
    if (gate == NULL) {
        failure = "could not open the download gate";
        goto cleanup;
    }
    fclose(gate);

    if (!updater_wait_past(&loop, &updater, MESH_UPDATE_DOWNLOADING)) {
        failure = "the download never finished";
        goto cleanup;
    }
    /* And once the download is behind us the fraction is gone rather than left at its last
       reading: a step with no length is reported as one, not as a stale number. */
    permille = 999U;
    if (mesh_updater_progress(&updater, &permille) || permille != 0U) {
        failure = "progress should be unknown once the download is over";
        goto cleanup;
    }
    if (updater.state != MESH_UPDATE_READY) {
        failure =
            updater.message[0] != '\0' ? updater.message : "a verified download should install";
        goto cleanup;
    }

    /* The binary is in place, executable, and byte-for-byte what was served. */
    struct stat info;
    if (stat(installed_binary, &info) != 0 || (info.st_mode & 0111) == 0 ||
        (size_t)info.st_size != sizeof k_payload - 1U) {
        failure = "the installed binary should be in place and executable";
        goto cleanup;
    }
    uint8_t installed[INKWELL_SHA256_DIGEST_LEN];
    if (inkwell_sha256_file(installed_binary, installed) != 0 ||
        memcmp(installed, binary_digest, sizeof binary_digest) != 0) {
        failure = "the installed binary should hash to what the release claimed";
        goto cleanup;
    }
    /* And nothing is left staged next to it. */
    char staged[300];
    snprintf(staged, sizeof staged, "%s.update", install_path);
    if (access(staged, F_OK) == 0) {
        failure = "the staging file should be gone once installed";
        goto cleanup;
    }

    /* The pak's own version has moved with the binary. Without this the Pak Store would read
       a pak.json still claiming the old version and offer an update the device already has -
       and only the `version` value changes, so the rest of the file survives untouched. */
    char pak_body[512];
    FILE *reread = fopen(pak_json_path, "rb");
    size_t pak_len = reread != NULL ? fread(pak_body, 1U, sizeof pak_body - 1U, reread) : 0U;
    if (reread != NULL) {
        fclose(reread);
    }
    pak_body[pak_len] = '\0';
    if (strstr(pak_body, "\"version\": \"v999.0.0\"") == NULL ||
        strstr(pak_body, "\"name\": \"MeshClient\"") == NULL ||
        strstr(pak_body, "\"type\": \"TOOL\"") == NULL) {
        failure = "the install should stamp the pak.json version and leave the rest alone";
        goto cleanup;
    }

    /*
     * Now the case that matters most: a release whose digest does not match what arrives. The
     * download must be discarded and the installed binary left exactly as it was, because this
     * check is the only thing standing between the client and running someone else's code.
     */
    json = fopen(json_path, "wb");
    if (json == NULL) {
        failure = "could not rewrite the release json";
        goto cleanup;
    }
    fprintf(json,
            "{\"tag_name\":\"v999.0.1\",\"assets\":[{\"name\":\"meshclient-tg5040-aarch64\","
            "\"size\":%zu,\"browser_download_url\":\"https://github.com/mcereal/mesh-client/"
            "releases/download/v999.0.1/meshclient-tg5040-aarch64\",\"digest\":\"sha256:%064d\"}]}",
            payload_size, 0);
    fclose(json);

    updater.state = MESH_UPDATE_IDLE;
    if (mesh_updater_check(&updater, 0U) != 0 ||
        !updater_wait_past(&loop, &updater, MESH_UPDATE_CHECKING) ||
        updater.asset_sha256[0] == '\0') {
        failure = "the second check should also read the release metadata";
        goto cleanup;
    }
    updater.state = MESH_UPDATE_AVAILABLE;
    if (mesh_updater_install(&updater, 0U) != 0 ||
        !updater_wait_past(&loop, &updater, MESH_UPDATE_DOWNLOADING)) {
        failure = "the second install should run";
        goto cleanup;
    }
    if (updater.state != MESH_UPDATE_FAILED) {
        failure = "a mismatched checksum must fail the install";
        goto cleanup;
    }
    if (access(staged, F_OK) == 0) {
        failure = "a rejected download should not be left on disk";
        goto cleanup;
    }
    /* The previously installed binary is untouched: a bad update never damages a good one. */
    if (inkwell_sha256_file(installed_binary, installed) != 0 ||
        memcmp(installed, binary_digest, sizeof binary_digest) != 0) {
        failure = "a rejected download must leave the installed binary alone";
        goto cleanup;
    }
    /* And so is the version it advertises - nothing was installed to advertise. */
    reread = fopen(pak_json_path, "rb");
    pak_len = reread != NULL ? fread(pak_body, 1U, sizeof pak_body - 1U, reread) : 0U;
    if (reread != NULL) {
        fclose(reread);
    }
    pak_body[pak_len] = '\0';
    if (strstr(pak_body, "\"version\": \"v999.0.0\"") == NULL) {
        failure = "a rejected download must leave pak.json alone";
        goto cleanup;
    }

cleanup:
    if (updater_up) {
        mesh_updater_shutdown(&updater);
    }
    if (loop_up) {
        inkwell_loop_shutdown(&loop);
    }
    https_fixture_stop(&server);
    unlink(payload_path);
    unlink(json_path);
    unlink(gate_path);
    unlink(install_path);
    unlink(pak_json_path);
#if defined(__APPLE__)
    {
        char tidy[640];
        if ((size_t)snprintf(tidy, sizeof tidy, "rm -rf '%s' '%s/new'", install_path, dir) <
            sizeof tidy) {
            (void)system(tidy);
        }
    }
#endif
    rmdir(shared_dir);
    rmdir(bin_dir);
    rmdir(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Answers the handshake and then nothing at all, the way a server behind a dead proxy does. */
static void updater_serve_nothing(void *userdata, const struct https_fixture_request *request,
                                  struct https_fixture_conn *conn) {
    (void)userdata;
    (void)request;
    (void)conn;
    sleep(120);
}

/*
 * A server that takes the request and never answers must not wedge the client.
 *
 * The deadline is the updater's own and it is checked on the tick, so a reply that never comes
 * cannot hold the About screen at "checking" - and the connection is let go with it rather than
 * left open for the next check to trip over. The clock is walked forward so the timeout is
 * reached in a handful of turns rather than in real time.
 */
MESH_TEST_CASE(updater_gives_up_on_a_silent_server, unit) {
    const char *failure = NULL;
    struct https_fixture server;
    memset(&server, 0, sizeof server);
    struct inkwell_loop loop;
    struct mesh_updater updater;
    bool loop_up = false;
    bool updater_up = false;

    if (!https_fixture_start(&server, updater_serve_nothing, NULL)) {
        failure = "could not stand up the server";
        goto cleanup;
    }
    if (inkwell_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (mesh_updater_init(&updater, &loop) != 0) {
        failure = "updater init failed";
        goto cleanup;
    }
    updater_up = true;
    https_fixture_attach(&server, &updater.fetch);
    /* Never let anything rename over the running test binary. */
    snprintf(updater.install_path, sizeof updater.install_path, "%s", "/nonexistent/meshclient");
    if (mesh_updater_check(&updater, 0U) != 0) {
        failure = "check should start";
        goto cleanup;
    }

    for (int i = 0; i < 20 && updater.state == MESH_UPDATE_CHECKING; ++i) {
        inkwell_loop_run(&loop, 10);
        mesh_updater_tick(&updater, (uint64_t)i * 5000U);
    }
    if (updater.state != MESH_UPDATE_FAILED) {
        failure = "a server that never answers should hit the timeout";
        goto cleanup;
    }
    if (inkwell_fetch_busy(&updater.fetch)) {
        failure = "the timed-out request should have let its connection go";
        goto cleanup;
    }

cleanup:
    if (updater_up) {
        mesh_updater_shutdown(&updater);
    }
    if (loop_up) {
        inkwell_loop_shutdown(&loop);
    }
    https_fixture_stop(&server);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A server that is not there at all says which way it is not there, and names the host.
 *
 * "Could not reach GitHub" is what this said for every network failure until inkwell's fetcher
 * reported which one it was. A refused connect is the case a suite can produce on demand.
 */
MESH_TEST_CASE(updater_says_which_network_failure, unit) {
    const char *failure = NULL;
    struct inkwell_loop loop;
    struct mesh_updater updater;
    bool loop_up = false;
    bool updater_up = false;

    if (inkwell_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (mesh_updater_init(&updater, &loop) != 0) {
        failure = "updater init failed";
        goto cleanup;
    }
    updater_up = true;
    /* Nothing listens on port 1. */
    inkwell_fetch_connect_to(&updater.fetch, "127.0.0.1", 1U);
    snprintf(updater.install_path, sizeof updater.install_path, "%s", "/nonexistent/meshclient");
    if (mesh_updater_check(&updater, 0U) != 0) {
        failure = "check should start";
        goto cleanup;
    }
    if (!updater_wait_past(&loop, &updater, MESH_UPDATE_CHECKING) ||
        updater.state != MESH_UPDATE_FAILED) {
        failure = "a refused connection should fail the check";
        goto cleanup;
    }
    {
        char want[MESH_UPDATE_MESSAGE_MAX];
        (void)inkcell_str_format(want, sizeof want, MESH_STR_LINK_UNREACHABLE, "api.github.com",
                                 strerror(ECONNREFUSED));
        if (strcmp(updater.message, want) != 0) {
            failure = "and say it was refused, by which host, rather than only that it failed";
            goto cleanup;
        }
    }

cleanup:
    if (updater_up) {
        mesh_updater_shutdown(&updater);
    }
    if (loop_up) {
        inkwell_loop_shutdown(&loop);
    }
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

#endif /* INKWELL_HAVE_TLS */

/*
 * A build that was not stamped by the release script must never look like a release, whatever
 * its version string says. This is the safeguard that keeps the updater from replacing a
 * binary someone just built with `make brick`, and the test suite is exactly such a build.
 */
MESH_TEST_CASE(version_build_stamp, unit) {
#ifdef MESHCLIENT_RELEASE_BUILD
    record_failure(test_name, "the test build should not be stamped as a release");
    return;
#else
    MESH_TEST_FAIL_IF(mesh_version_is_release(),
                      "an unstamped build must not report itself as a release");
    /* It still reports a useful number, suffixed so it is obvious on screen. */
    const char *version = mesh_version_string();
    const size_t len = strlen(version);
    MESH_TEST_FAIL_IF(len < 4U || strcmp(version + len - 4U, "-dev") != 0, version);
    /* And "-dev" is a prerelease of the version it names, so it sorts below the real thing. */
    char base[MESH_UPDATE_VERSION_MAX];
    snprintf(base, sizeof base, "%.*s", (int)(len - 4U), version);
    MESH_TEST_FAIL_IF(inkwell_version_compare(version, base) >= 0,
                      "a -dev build should sort below the release it precedes");
    /* No release, however new, is ever offered to an unstamped build. */
    MESH_TEST_FAIL_IF(mesh_version_is_newer_than_running("999.0.0"),
                      "an unstamped build must never be offered an update");
    record_success(test_name);
#endif
}

/*
 * One antenna: the Brick's Wi-Fi and Bluetooth are one Xradio part, and a Meshtastic node ends
 * the link after a second of silence. An install pressed over a live link was measured
 * producing the first FromRadio failure 36 ms later, so the download and the radio take turns.
 *
 * What is pinned here is that the answer is *derived* from the updater's own state. A flag would
 * work until a download failed holding it, and a radio that will not reconnect after a failed
 * update is a worse bug than the one this fixes.
 */
MESH_TEST_CASE(updater_download_holds_the_radio, unit) {
    struct mesh_updater updater;
    memset(&updater, 0, sizeof updater);

    MESH_TEST_FAIL_IF(mesh_updater_holds_the_radio(NULL), "no updater cannot hold anything");

    const enum mesh_update_state releases[] = {
        MESH_UPDATE_IDLE,      MESH_UPDATE_CHECKING, MESH_UPDATE_UP_TO_DATE,
        MESH_UPDATE_AVAILABLE, MESH_UPDATE_READY,    MESH_UPDATE_FAILED,
    };
    for (size_t i = 0; i < sizeof releases / sizeof releases[0]; ++i) {
        updater.state = releases[i];
        MESH_TEST_FAIL_IF(mesh_updater_holds_the_radio(&updater),
                          mesh_update_state_name(releases[i]));
    }

    /* Fetching the asset, and hashing it - which wants no antenna itself, but sits between the
       download and a relaunch, so a link brought up for it could only lose the sync it began. */
    updater.state = MESH_UPDATE_DOWNLOADING;
    MESH_TEST_FAIL_IF(!mesh_updater_holds_the_radio(&updater), "a download must hold the radio");
    updater.state = MESH_UPDATE_VERIFYING;
    MESH_TEST_FAIL_IF(!mesh_updater_holds_the_radio(&updater), "verifying must hold the radio");

    /* The release is the failure, not a separate thing anyone has to remember to do. */
    updater.state = MESH_UPDATE_FAILED;
    MESH_TEST_FAIL_IF(mesh_updater_holds_the_radio(&updater),
                      "a failed download must not strand the radio");
    record_success(test_name);
}
