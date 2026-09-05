#define _POSIX_C_SOURCE 200809L

/* Version comparison and the self-update lifecycle. */

#include "framework/mesh_test.h"

#include "mesh/core/event_loop.h"
#include "mesh/core/updater.h"
#include "mesh/core/version.h"
#include "mesh/utils/sha256.h"

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
   moved: every step is driven by a child process, so the test has to pump the loop. */
static bool updater_wait_past(struct mesh_event_loop *loop, struct mesh_updater *updater,
                              enum mesh_update_state from) {
    for (int i = 0; i < 200 && updater->state == from; ++i) {
        mesh_event_loop_run(loop, 50);
        mesh_updater_tick(updater, (uint64_t)i * 50U);
    }
    return updater->state != from;
}

/* ---- client version and self-update ------------------------------------------------------- */

MESH_TEST_CASE(version_compare, unit) {
    /* Ordering: each pair must compare strictly less-than in the given direction. */
    static const struct {
        const char *lower;
        const char *higher;
    } k_ordered[] = {
        {"1.0.0", "1.0.1"},
        {"1.0.9", "1.1.0"},
        {"1.9.0", "2.0.0"},
        {"1.2.0", "1.10.0"},     /* not string order */
        {"1.2.0-rc.1", "1.2.0"}, /* a prerelease precedes its release */
        {"1.2.0-beta.1", "1.2.0-beta.2"},
        {"1.2.0-beta.2", "1.2.0-beta.10"}, /* numeric identifiers compare numerically */
        {"1.2.0-beta", "1.2.0-rc"},
        {"1.2.0-rc.1", "1.2.0-rc.1.1"}, /* a longer run of identifiers outranks its prefix */
        {"garbage", "1.0.0"},           /* unparseable can never look newer */
    };
    for (size_t i = 0; i < sizeof k_ordered / sizeof k_ordered[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_version_compare(k_ordered[i].lower, k_ordered[i].higher) >= 0,
                          k_ordered[i].lower);
        MESH_TEST_FAIL_IF(mesh_version_compare(k_ordered[i].higher, k_ordered[i].lower) <= 0,
                          "the reverse comparison should be positive");
    }

    /* Equality, including the forms a GitHub tag and CMake spell differently. */
    static const char *const k_equal[][2] = {
        {"1.2.3", "1.2.3"},
        {"v1.2.3", "1.2.3"},
        {"V1.2.3", "v1.2.3"},
        {"1.2", "1.2.0"},
        {"1", "1.0.0"},
        {"1.2.3+build7", "1.2.3"}, /* build metadata is not part of precedence */
        {"1.2.3-rc.1+build7", "1.2.3-rc.1"},
    };
    for (size_t i = 0; i < sizeof k_equal / sizeof k_equal[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_version_compare(k_equal[i][0], k_equal[i][1]) != 0, k_equal[i][0]);
    }

    MESH_TEST_FAIL_IF(mesh_version_compare(NULL, NULL) != 0 ||
                          mesh_version_compare("1.0.0", NULL) <= 0 ||
                          mesh_version_compare(NULL, "1.0.0") >= 0,
                      "NULL should be handled and sort below a real version");

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
    struct mesh_event_loop loop;
    MESH_TEST_FAIL_IF(mesh_event_loop_init(&loop) != 0, "event loop init failed");

    struct mesh_updater updater;
    if (mesh_updater_init(&updater, &loop) != 0) {
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "updater init failed");
        return;
    }
    if (updater.state != MESH_UPDATE_IDLE || updater.revision != 0U) {
        mesh_updater_shutdown(&updater);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "a fresh updater should be idle");
        return;
    }
    /* init reads /proc/self/exe, so the staged name must sit beside the running binary - the
       rename that installs it is only atomic within one directory. */
    if (updater.install_path[0] == '\0' ||
        strncmp(updater.staged_path, updater.install_path, strlen(updater.install_path)) != 0) {
        mesh_updater_shutdown(&updater);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "the staged path should sit next to the installed one");
        return;
    }

    /* Install is only reachable from AVAILABLE with an asset in hand; from IDLE it is a
       programming error, not a no-op that silently downloads nothing. */
    if (mesh_updater_install(&updater, 0U) == 0) {
        mesh_updater_shutdown(&updater);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "install from idle should be refused");
        return;
    }

    /* An updater with no event loop reports itself unavailable rather than half-working. */
    struct mesh_updater detached;
    mesh_updater_init(&detached, NULL);
    if (mesh_updater_available(&detached) || mesh_updater_check(&detached, 0U) != -ENOTSUP) {
        mesh_updater_shutdown(&detached);
        mesh_updater_shutdown(&updater);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "an updater with no loop should be unavailable");
        return;
    }
    mesh_updater_shutdown(&detached);

    /* tick() on an idle updater must not touch a child it does not have. */
    mesh_updater_tick(&updater, 1000000U);
    if (updater.state != MESH_UPDATE_IDLE) {
        mesh_updater_shutdown(&updater);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "ticking an idle updater should change nothing");
        return;
    }

    if (mesh_update_state_name(MESH_UPDATE_READY) == NULL ||
        strcmp(mesh_update_state_name(MESH_UPDATE_IDLE), "idle") != 0) {
        mesh_updater_shutdown(&updater);
        mesh_event_loop_shutdown(&loop);
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
        mesh_event_loop_shutdown(&loop);
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
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, dev_failure);
        return;
    }

    mesh_updater_shutdown(&updater);
    mesh_event_loop_shutdown(&loop);
    record_success(test_name);
}

/*
 * The whole update path with a fake `curl` on PATH: fork, drain its stdout through the event
 * loop, parse the release, download, verify the checksum and rename the binary into place.
 *
 * Worth doing for real rather than mocking the pieces, because the bugs this path attracts are
 * in the seams - a child reaped before its output was drained, a blocking waitpid in the
 * loop's own thread - and none of those show up when the fetch is stubbed out.
 */
/*
 * The CA bundle the fetcher is pointed at.
 *
 * Worth pinning because getting it wrong is invisible until it is on hardware: the Brick has no
 * system CA store, so a build that resolves nothing here fails every check with curl exit 60 and
 * self-update simply never works. The environment override is the one branch a test can drive
 * deterministically - the pak lookup needs the binary to live in a pak, and the system paths
 * differ per distro - and it is also the branch that documents the precedence.
 */
MESH_TEST_CASE(updater_ca_bundle, unit) {
    char bundle_path[] = "/tmp/meshclient_ca_XXXXXX";
    const int fd = mkstemp(bundle_path);
    if (fd < 0) {
        record_failure(test_name, "could not create a stand-in CA bundle");
        return;
    }
    (void)!write(fd, "# not a real bundle\n", 20U);
    close(fd);

    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        unlink(bundle_path);
        record_failure(test_name, "event loop init failed");
        return;
    }

    /* SSL_CERT_FILE wins over everything: whatever the host has in /etc/ssl, an operator who
       names a bundle gets that bundle. */
    setenv("SSL_CERT_FILE", bundle_path, 1);
    struct mesh_updater updater;
    mesh_updater_init(&updater, &loop);
    const bool honoured = strcmp(updater.ca_bundle, bundle_path) == 0;
    mesh_updater_shutdown(&updater);
    unsetenv("SSL_CERT_FILE");
    if (!honoured) {
        mesh_event_loop_shutdown(&loop);
        unlink(bundle_path);
        record_failure(test_name, "SSL_CERT_FILE should be the bundle the fetcher is given");
        return;
    }

    /* A path that does not exist is ignored rather than passed to curl, which would turn a
       stale environment variable into a failed update with a confusing message. */
    setenv("CURL_CA_BUNDLE", "/nonexistent/meshclient/ca.crt", 1);
    mesh_updater_init(&updater, &loop);
    const bool ignored = strcmp(updater.ca_bundle, "/nonexistent/meshclient/ca.crt") != 0;
    mesh_updater_shutdown(&updater);
    unsetenv("CURL_CA_BUNDLE");
    mesh_event_loop_shutdown(&loop);
    unlink(bundle_path);
    if (!ignored) {
        record_failure(test_name, "an unreadable CA bundle should not be used");
        return;
    }

    record_success(test_name);
}

MESH_TEST_CASE(updater_fetch_and_install, unit) {
    char dir[] = "/tmp/meshclient_update_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");
    const char *failure = NULL;
    char *saved_path = NULL;
    struct mesh_event_loop loop;
    struct mesh_updater updater;
    bool loop_up = false;
    bool updater_up = false;

    char payload_path[256];
    char json_path[256];
    char curl_path[256];
    char install_path[256];
    char bin_dir[256];
    char shared_dir[256];
    char pak_json_path[256];
    snprintf(payload_path, sizeof payload_path, "%s/payload", dir);
    snprintf(json_path, sizeof json_path, "%s/release.json", dir);
    snprintf(curl_path, sizeof curl_path, "%s/curl", dir);
    /* The pak layout, because the install stamps the pak.json two directories above the
       binary and would find nothing in a flat one. */
    snprintf(bin_dir, sizeof bin_dir, "%s/bin", dir);
    snprintf(shared_dir, sizeof shared_dir, "%s/bin/shared", dir);
    snprintf(install_path, sizeof install_path, "%s/bin/shared/meshclient", dir);
    snprintf(pak_json_path, sizeof pak_json_path, "%s/pak.json", dir);
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

    uint8_t digest[MESH_SHA256_DIGEST_LEN];
    char digest_hex[MESH_SHA256_HEX_LEN];
    if (mesh_sha256_file(payload_path, digest) != 0) {
        failure = "could not hash the payload";
        goto cleanup;
    }
    mesh_sha256_hex(digest, digest_hex, sizeof digest_hex);

    FILE *json = fopen(json_path, "wb");
    if (json == NULL) {
        failure = "could not write the release json";
        goto cleanup;
    }
    fprintf(json,
            "{\"tag_name\":\"v999.0.0\",\"assets\":[{\"name\":\"meshclient-tg5040-aarch64\","
            "\"size\":%zu,\"browser_download_url\":\"https://github.com/mcereal/mesh-client/"
            "releases/download/v999.0.0/meshclient-tg5040-aarch64\",\"digest\":\"sha256:%s\"}]}",
            sizeof k_payload - 1U, digest_hex);
    fclose(json);

    /* A stand-in for curl: with -o it "downloads" the payload, otherwise it prints the
       release metadata on stdout, which is exactly the shape the real one is invoked in. */
    FILE *script = fopen(curl_path, "w");
    if (script == NULL) {
        failure = "could not write the fake curl";
        goto cleanup;
    }
    fprintf(script,
            "#!/bin/sh\n"
            "out=''\n"
            "prev=''\n"
            "for a in \"$@\"; do\n"
            "  if [ \"$prev\" = '-o' ]; then out=\"$a\"; fi\n"
            "  prev=\"$a\"\n"
            "done\n"
            "if [ -n \"$out\" ]; then cp '%s' \"$out\"; else cat '%s'; fi\n",
            payload_path, json_path);
    fclose(script);
    if (chmod(curl_path, 0755) != 0) {
        failure = "could not make the fake curl executable";
        goto cleanup;
    }

    const char *old_path = getenv("PATH");
    saved_path = old_path != NULL ? strdup(old_path) : NULL;
    char new_path[1024];
    snprintf(new_path, sizeof new_path, "%s:%s", dir, old_path != NULL ? old_path : "/usr/bin");
    setenv("PATH", new_path, 1);

    if (mesh_event_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (mesh_updater_init(&updater, &loop) != 0) {
        failure = "updater init failed";
        goto cleanup;
    }
    updater_up = true;
    if (updater.fetcher == NULL || strcmp(updater.fetcher, "curl") != 0) {
        failure = "the fake curl should have been picked up from PATH";
        goto cleanup;
    }

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
        updater.asset_size != sizeof k_payload - 1U) {
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
    if (!updater_wait_past(&loop, &updater, MESH_UPDATE_DOWNLOADING)) {
        failure = "the download never finished";
        goto cleanup;
    }
    if (updater.state != MESH_UPDATE_READY) {
        failure =
            updater.message[0] != '\0' ? updater.message : "a verified download should install";
        goto cleanup;
    }

    /* The binary is in place, executable, and byte-for-byte what was served. */
    struct stat info;
    if (stat(install_path, &info) != 0 || (info.st_mode & 0111) == 0 ||
        (size_t)info.st_size != sizeof k_payload - 1U) {
        failure = "the installed binary should be in place and executable";
        goto cleanup;
    }
    uint8_t installed[MESH_SHA256_DIGEST_LEN];
    if (mesh_sha256_file(install_path, installed) != 0 ||
        memcmp(installed, digest, sizeof digest) != 0) {
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
            sizeof k_payload - 1U, 0);
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
    if (mesh_sha256_file(install_path, installed) != 0 ||
        memcmp(installed, digest, sizeof digest) != 0) {
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
        mesh_event_loop_shutdown(&loop);
    }
    if (saved_path != NULL) {
        setenv("PATH", saved_path, 1);
        free(saved_path);
    }
    unlink(payload_path);
    unlink(json_path);
    unlink(curl_path);
    unlink(install_path);
    unlink(pak_json_path);
    rmdir(shared_dir);
    rmdir(bin_dir);
    rmdir(dir);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

/*
 * A downloader that closes stdout but keeps running must not wedge the client.
 *
 * The trap: once the child closes its stdout, epoll reports EOF/HUP on that fd on every wait,
 * so mesh_event_loop_run() never sees a zero-event timeout and never returns - which means
 * mesh_updater_tick() never runs and the timeout that is supposed to kill the child never
 * fires. The whole UI is single-threaded, so that is a freeze, not a slow update. The fd is
 * therefore dropped at EOF while the pid is kept for polling.
 */
MESH_TEST_CASE(updater_child_outlives_stdout, unit) {
    char dir[] = "/tmp/meshclient_hang_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");
    const char *failure = NULL;
    char *saved_path = NULL;
    struct mesh_event_loop loop;
    struct mesh_updater updater;
    bool loop_up = false;
    bool updater_up = false;

    char curl_path[256];
    snprintf(curl_path, sizeof curl_path, "%s/curl", dir);
    FILE *script = fopen(curl_path, "w");
    if (script == NULL) {
        failure = "could not write the fake curl";
        goto cleanup;
    }
    /* Closes stdout immediately, then lingers well past the test's budget. */
    fprintf(script, "#!/bin/sh\nexec >&-\nsleep 120\n");
    fclose(script);
    if (chmod(curl_path, 0755) != 0) {
        failure = "could not make the fake curl executable";
        goto cleanup;
    }

    const char *old_path = getenv("PATH");
    saved_path = old_path != NULL ? strdup(old_path) : NULL;
    char new_path[1024];
    snprintf(new_path, sizeof new_path, "%s:%s", dir, old_path != NULL ? old_path : "/usr/bin");
    setenv("PATH", new_path, 1);

    if (mesh_event_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (mesh_updater_init(&updater, &loop) != 0) {
        failure = "updater init failed";
        goto cleanup;
    }
    updater_up = true;
    if (mesh_updater_check(&updater, 0U) != 0) {
        failure = "check should start";
        goto cleanup;
    }

    /*
     * Each turn must return promptly. Before the fix mesh_event_loop_run() spun forever inside
     * its own loop and this never came back at all; the deadline is walked forward so the
     * timeout can be reached in a handful of turns rather than in real time.
     */
    for (int i = 0; i < 20 && updater.state == MESH_UPDATE_CHECKING; ++i) {
        mesh_event_loop_run(&loop, 10);
        mesh_updater_tick(&updater, (uint64_t)i * 5000U);
    }
    if (updater.state != MESH_UPDATE_FAILED) {
        failure = "a child that closed stdout without exiting should hit the timeout";
        goto cleanup;
    }
    /* And the child is gone rather than left behind holding the staging file. */
    if (updater.child > 0 || updater.child_fd >= 0) {
        failure = "the timed-out child should have been reaped and its pipe closed";
        goto cleanup;
    }

cleanup:
    if (updater_up) {
        mesh_updater_shutdown(&updater);
    }
    if (loop_up) {
        mesh_event_loop_shutdown(&loop);
    }
    if (saved_path != NULL) {
        setenv("PATH", saved_path, 1);
        free(saved_path);
    }
    unlink(curl_path);
    rmdir(dir);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

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
    MESH_TEST_FAIL_IF(mesh_version_compare(version, base) >= 0,
                      "a -dev build should sort below the release it precedes");
    /* No release, however new, is ever offered to an unstamped build. */
    MESH_TEST_FAIL_IF(mesh_version_is_newer_than_running("999.0.0"),
                      "an unstamped build must never be offered an update");
    record_success(test_name);
#endif
}
