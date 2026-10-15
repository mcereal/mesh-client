#define _POSIX_C_SOURCE 200809L

/*
 * A Tern node's firmware, from the project's feed to the node running it.
 *
 * The feed is one document, read by pure functions, and the first cases hold the parser to what
 * the phone apps accept of it. The rest are the check and the install against an HTTPS server on
 * loopback serving a feed and an image of the suite's own, and a node played here frame by frame
 * through the client's own struct mesh_tern: the check names the image for the node's board and
 * region, the download is held to the feed's digest, and the image goes down the companion link
 * - over no bus of its own - until the node is back saying the new release.
 */

#include "framework/mesh_test.h"

#include "inkwell/base/text.h"
#include "inkwell/codec/sha256.h"
#include "inkwell/runtime/loop.h"
#include "mesh/core/firmware.h"
#include "mesh/core/firmware_tern.h"
#include "mesh/core/firmware_update.h"
#include "mesh/core/session.h"
#include "mesh/core/tern.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TERN_DIGEST "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define TERN_BASE "https://ternmesh.org/firmware/"

static const char k_feed[] =
    "{\"release\":\"0.5.0-alpha.1\",\"notes\":{\"ignored\":[1,2]},\"images\":["
    "{\"board\":\"heltec-v3\",\"region\":\"EU868\","
    "\"file\":\"tern-heltec-v3-eu868-0.5.0-alpha.1-app.bin\",\"size\":773264,"
    "\"sha256\":\"" TERN_DIGEST "\"},"
    "{\"board\":\"heltec-v3\",\"region\":\"US915\","
    "\"file\":\"tern-heltec-v3-us915-0.5.0-alpha.1-app.bin\",\"size\":773280,"
    "\"sha256\":\"0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF\","
    "\"later\":true},"
    /* Not the shape the apps accept: a path, no size, a short digest. Each is skipped. */
    "{\"board\":\"heltec-v3\",\"region\":\"AU915\",\"file\":\"../tern.bin\",\"size\":1,"
    "\"sha256\":\"" TERN_DIGEST "\"},"
    "{\"board\":\"heltec-v3\",\"region\":\"NZ915\",\"file\":\"tern.bin\",\"size\":0,"
    "\"sha256\":\"" TERN_DIGEST "\"},"
    "{\"board\":\"heltec-v3\",\"region\":\"CL915\",\"file\":\"tern.bin\",\"size\":10,"
    "\"sha256\":\"0123\"},"
    "{\"board\":\"heltec-v3\",\"region\":\"MY919\",\"file\":\"tern.bin\",\"size\":10,"
    "\"sha256\":\"" TERN_DIGEST "\"},"
    "{\"board\":\"heltec-v3\",\"region\":\"MY919\",\"file\":\"tern-again.bin\",\"size\":10,"
    "\"sha256\":\"" TERN_DIGEST "\"}]}";

/* The feed's image for a board and region, case-insensitively, and nothing for an empty half. */
MESH_TEST_CASE(firmware_tern_feed_names_the_image_for_board_and_region, unit) {
    struct mesh_firmware_boards boards;
    struct mesh_firmware_release release;
    MESH_TEST_FAIL_IF(
        !mesh_firmware_tern_parse(k_feed, 0U, "heltec-v3", "EU868", TERN_BASE, &boards, &release),
        "the feed should parse");
    MESH_TEST_FAIL_IF(strcmp(release.version, "0.5.0-alpha.1") != 0, "it names its release");
    MESH_TEST_FAIL_IF(boards.found != 1U || boards.count != 1U ||
                          strcmp(boards.entries[0].target, "heltec-v3") != 0 ||
                          boards.entries[0].path != MESH_FIRMWARE_PATH_LINK ||
                          !boards.entries[0].actively_supported ||
                          boards.entries[0].architecture[0] != '\0',
                      "one board, over the link, with no chip of its own to check");
    MESH_TEST_FAIL_IF(
        strcmp(release.image_url, TERN_BASE "tern-heltec-v3-eu868-0.5.0-alpha.1-app.bin") != 0 ||
            strcmp(release.image_name, "tern-heltec-v3-eu868-0.5.0-alpha.1-app.bin") != 0 ||
            release.image_bytes != 773264U || strcmp(release.image_sha256, TERN_DIGEST) != 0 ||
            release.manifest_url[0] != '\0' || release.wipe,
        "the file sits beside the feed, with its size and digest, and is an application");

    MESH_TEST_FAIL_IF(
        !mesh_firmware_tern_parse(k_feed, 0U, "HELTEC-V3", "us915", TERN_BASE, &boards, &release) ||
            boards.found != 1U || release.image_bytes != 773280U ||
            strcmp(release.image_sha256, TERN_DIGEST) != 0,
        "board and region match in any case, and the digest is lowered");

    MESH_TEST_FAIL_IF(
        !mesh_firmware_tern_parse(k_feed, 0U, "heltec-v3", "", TERN_BASE, &boards, &release) ||
            boards.found != 0U || release.image_url[0] != '\0' ||
            strcmp(release.version, "0.5.0-alpha.1") != 0,
        "a node with no region set takes no image, and still hears the release");
    MESH_TEST_FAIL_IF(
        !mesh_firmware_tern_parse(k_feed, 0U, "", "EU868", TERN_BASE, &boards, &release) ||
            boards.found != 0U,
        "nor does a build that cannot be updated over its link");
    MESH_TEST_FAIL_IF(
        !mesh_firmware_tern_parse(k_feed, 0U, "t-beam", "EU868", TERN_BASE, &boards, &release) ||
            boards.found != 0U,
        "a board the feed does not list takes nothing");

    static const char *const k_skipped[] = {"AU915", "NZ915", "CL915"};
    for (size_t i = 0; i < sizeof k_skipped / sizeof k_skipped[0]; ++i) {
        MESH_TEST_FAIL_IF(!mesh_firmware_tern_parse(k_feed, 0U, "heltec-v3", k_skipped[i],
                                                    TERN_BASE, &boards, &release) ||
                              boards.found != 0U,
                          "an entry with a path, no size or a short digest is skipped");
    }
    MESH_TEST_FAIL_IF(
        !mesh_firmware_tern_parse(k_feed, 0U, "heltec-v3", "MY919", TERN_BASE, &boards, &release) ||
            boards.found != 2U || boards.count != 0U || release.image_url[0] != '\0',
        "two images for one board and region are the ambiguous answer, not either");
    MESH_TEST_FAIL_IF(!mesh_firmware_tern_parse(k_feed, 0U, "heltec-v3", "EU868",
                                                "http://ternmesh.org/firmware/", &boards,
                                                &release) ||
                          boards.found != 0U,
                      "an image is fetched over HTTPS or not at all");

    MESH_TEST_FAIL_IF(mesh_firmware_tern_parse("{\"release\":\"latest\",\"images\":[]}", 0U,
                                               "heltec-v3", "EU868", TERN_BASE, &boards, &release),
                      "a release that is not SemVer cannot be ordered, so it is not a release");
    MESH_TEST_FAIL_IF(mesh_firmware_tern_parse("{\"release\":\"0.5.0\"}", 0U, "heltec-v3", "EU868",
                                               TERN_BASE, &boards, &release),
                      "a feed with no images is not the feed");
    MESH_TEST_FAIL_IF(
        mesh_firmware_tern_parse("[]", 0U, "heltec-v3", "EU868", TERN_BASE, &boards, &release),
        "nor is something that is not an object");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_tern_releases_order_as_semver, unit) {
    MESH_TEST_FAIL_IF(mesh_firmware_tern_version_compare("0.4.0-alpha.1", "0.5.0-alpha.1") >= 0,
                      "0.4 is behind 0.5");
    MESH_TEST_FAIL_IF(mesh_firmware_tern_version_compare("0.5.0-alpha.1", "0.5.0") >= 0,
                      "a prerelease is behind the release it leads to");
    MESH_TEST_FAIL_IF(mesh_firmware_tern_version_compare("0.5.0-alpha.2", "0.5.0-alpha.1") <= 0,
                      "and prereleases order by their numbers");
    MESH_TEST_FAIL_IF(mesh_firmware_tern_version_compare("0.10.0", "0.9.0") <= 0,
                      "numbers order as numbers");
    MESH_TEST_FAIL_IF(mesh_firmware_tern_version_compare("0.5.0-alpha.1", "0.5.0-alpha.1") != 0,
                      "a release is its own");
    MESH_TEST_FAIL_IF(mesh_firmware_tern_version_compare("", "0.0.1") >= 0 ||
                          mesh_firmware_tern_version_compare(NULL, "0.0.1") >= 0,
                      "a node with no release is behind every release");
    record_success(test_name);
}

#ifdef INKWELL_HAVE_TLS

#include "support/https_fixture.h"

/* ---- the server: the feed and its image, out of the case's own directory --------------------- */

struct tern_cdn {
    char dir[64];
};

static void tern_serve(void *userdata, const struct https_fixture_request *request,
                       struct https_fixture_conn *conn) {
    const struct tern_cdn *const cdn = (const struct tern_cdn *)userdata;
    const char *const prefix = "/firmware/";
    const char *const name = request->target + strlen(prefix);
    if (strncmp(request->target, prefix, strlen(prefix)) != 0 || strchr(name, '/') != NULL ||
        name[0] == '\0') {
        https_fixture_reply(conn, 404, NULL, NULL, 0U);
        return;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/%s", cdn->dir, name);
    https_fixture_reply_file(conn, request, path);
}

#define TERN_IMAGE_LEN 1000U
#define TERN_IMAGE_NAME "tern-heltec-v3-eu868-0.5.0-alpha.1-app.bin"

static uint8_t g_image[TERN_IMAGE_LEN];
static struct mesh_session g_model;
static struct mesh_tern g_tern;

/* The node's side of the wire: only the last frame the client sent, which is all a node in
   lock-step needs to answer. */
struct tern_wire {
    uint8_t last[MESH_TERN_MAX_FRAME];
    size_t last_len;
    size_t count;
    size_t updates; /* UPDATE_BEGIN, _DATA and _END frames */
};

static int tern_wire_send(void *ctx, const uint8_t *frame, size_t len, uint32_t frame_id) {
    (void)frame_id;
    struct tern_wire *const wire = (struct tern_wire *)ctx;
    if (len > MESH_TERN_MAX_FRAME) {
        return -ENOBUFS;
    }
    memcpy(wire->last, frame, len);
    wire->last_len = len;
    wire->count++;
    if (frame[0] == MESH_TERN_UPDATE_BEGIN || frame[0] == MESH_TERN_UPDATE_DATA ||
        frame[0] == MESH_TERN_UPDATE_END) {
        wire->updates++;
    }
    return 0;
}

struct tern_harness {
    struct tern_cdn cdn;
    struct https_fixture server;
    struct inkwell_loop loop;
    struct mesh_firmware firmware;
    struct mesh_firmware_update update;
    struct tern_wire wire;
    struct mesh_protocol protocol;
    bool loop_up;
    bool firmware_up;
    bool update_up;
    uint64_t now_ms;
};

static void tern_node_says(struct tern_harness *harness, const struct mesh_tern_frame *frame) {
    uint8_t out[MESH_TERN_MAX_FRAME];
    const uint8_t version = g_tern.has_agreed ? g_tern.agreed : MESH_TERN_VERSION;
    const int len = mesh_tern_encode(frame, version, out, sizeof out);
    if (len > 0) {
        mesh_protocol_receive(&harness->protocol, out, (size_t)len);
    }
}

/* A bare answer, of `type`, to the request last on the wire. */
static void tern_node_answers(struct tern_harness *harness, uint8_t type) {
    const uint8_t frame[2] = {type, harness->wire.last[1]};
    mesh_protocol_receive(&harness->protocol, frame, sizeof frame);
}

/* HELLO answered as a heltec-v3 on `release`, in EU868: INFO, SET_TIME, a sync of SELF alone. */
static bool tern_node_greets(struct tern_harness *harness, const char *release) {
    if (harness->wire.last[0] != MESH_TERN_HELLO) {
        return false;
    }
    struct mesh_tern_frame info = {
        .type = MESH_TERN_INFO, .seq = harness->wire.last[1], .version = MESH_TERN_VERSION};
    inkwell_str_copy(info.text, sizeof info.text, "tern heltec-v3");
    info.text_len = (uint8_t)strlen(info.text);
    inkwell_str_copy(info.board, sizeof info.board, "heltec-v3");
    info.board_len = (uint8_t)strlen(info.board);
    inkwell_str_copy(info.release, sizeof info.release, release);
    info.release_len = (uint8_t)strlen(info.release);
    tern_node_says(harness, &info);
    tern_node_answers(harness, MESH_TERN_OK); /* SET_TIME */
    if (harness->wire.last[0] != MESH_TERN_SYNC) {
        return false;
    }
    struct mesh_tern_frame self = {.type = MESH_TERN_SELF, .seq = g_tern.news_expected};
    memset(self.address, 0x5a, sizeof self.address);
    inkwell_str_copy(self.text, sizeof self.text, "EU868");
    self.text_len = (uint8_t)strlen(self.text);
    tern_node_says(harness, &self);
    const struct mesh_tern_frame synced = {
        .type = MESH_TERN_SYNCED, .seq = harness->wire.last[1], .news = g_tern.news_expected};
    tern_node_says(harness, &synced);
    return mesh_tern_ready(&g_tern);
}

/* The link comes up again to the same node, now on `release`. */
static bool tern_node_reconnects(struct tern_harness *harness, const char *release) {
    memset(&harness->wire, 0, sizeof harness->wire);
    mesh_protocol_attach(&harness->protocol, tern_wire_send, &harness->wire);
    (void)mesh_protocol_begin(&harness->protocol);
    return tern_node_greets(harness, release);
}

/*
 * The node's half of an update, one request at a time: where to go on from, each chunk taken,
 * and the end - answered OK, or refused with `end_error` when it is not 0.
 */
static void tern_node_takes_updates(struct tern_harness *harness, uint8_t end_error) {
    for (int step = 0; step < 64; ++step) {
        switch (harness->wire.last[0]) {
        case MESH_TERN_UPDATE_BEGIN: {
            const struct mesh_tern_frame updating = {
                .type = MESH_TERN_UPDATING, .seq = harness->wire.last[1], .offset = 0U};
            tern_node_says(harness, &updating);
            break;
        }
        case MESH_TERN_UPDATE_DATA:
            tern_node_answers(harness, MESH_TERN_OK);
            break;
        case MESH_TERN_UPDATE_END:
            if (end_error != 0U) {
                const uint8_t refused[3] = {MESH_TERN_ERROR, harness->wire.last[1], end_error};
                mesh_protocol_receive(&harness->protocol, refused, sizeof refused);
            } else {
                tern_node_answers(harness, MESH_TERN_OK);
            }
            return;
        default:
            return;
        }
    }
}

static bool tern_write(const char *dir, const char *name, const void *data, size_t len) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *const file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    const bool written = fwrite(data, 1U, len, file) == len;
    return fclose(file) == 0 && written;
}

/* The feed, naming the image in the directory with `digest` - its own, or another. */
static bool tern_write_feed(const struct tern_harness *harness, const char *digest) {
    char feed[1024];
    const int len = snprintf(feed, sizeof feed,
                             "{\"release\":\"0.5.0-alpha.1\",\"images\":[{\"board\":\"heltec-v3\","
                             "\"region\":\"EU868\",\"file\":\"" TERN_IMAGE_NAME "\","
                             "\"size\":%u,\"sha256\":\"%s\"}]}",
                             TERN_IMAGE_LEN, digest);
    return len > 0 && tern_write(harness->cdn.dir, "latest.json", feed, (size_t)len);
}

static void tern_image_digest(char hex[INKWELL_SHA256_DIGEST_LEN * 2U + 1U]) {
    struct inkwell_sha256 sha;
    uint8_t digest[INKWELL_SHA256_DIGEST_LEN];
    inkwell_sha256_init(&sha);
    inkwell_sha256_update(&sha, g_image, sizeof g_image);
    inkwell_sha256_final(&sha, digest);
    inkwell_sha256_hex(digest, hex, INKWELL_SHA256_DIGEST_LEN * 2U + 1U);
}

static bool tern_harness_up(struct tern_harness *harness) {
    memset(harness, 0, sizeof *harness);
    snprintf(harness->cdn.dir, sizeof harness->cdn.dir, "%s", "/tmp/meshclient_tern_XXXXXX");
    if (mkdtemp(harness->cdn.dir) == NULL) {
        harness->cdn.dir[0] = '\0';
        return false;
    }
    for (size_t i = 0; i < sizeof g_image; ++i) {
        g_image[i] = (uint8_t)(i * 7U + 3U);
    }
    char digest[INKWELL_SHA256_DIGEST_LEN * 2U + 1U];
    tern_image_digest(digest);
    if (!tern_write(harness->cdn.dir, TERN_IMAGE_NAME, g_image, sizeof g_image) ||
        !tern_write_feed(harness, digest)) {
        return false;
    }
    if (!https_fixture_start(&harness->server, tern_serve, &harness->cdn)) {
        return false;
    }
    setenv("MESHCLIENT_FIRMWARE_TERN_URL", "https://example.invalid/firmware/latest.json", 1);
    setenv("MESHCLIENT_FIRMWARE_STAGING", harness->cdn.dir, 1);
    if (inkwell_loop_init(&harness->loop) != 0) {
        return false;
    }
    harness->loop_up = true;
    if (mesh_firmware_init(&harness->firmware, &harness->loop) != 0) {
        return false;
    }
    harness->firmware_up = true;
    if (mesh_firmware_update_init(&harness->update, &harness->loop) != 0) {
        return false;
    }
    harness->update_up = true;
    https_fixture_attach(&harness->server, &harness->firmware.fetch);
    https_fixture_attach(&harness->server, &harness->update.fetch);

    mesh_session_init(&g_model);
    mesh_tern_init(&g_tern, &g_model);
    harness->protocol = mesh_tern_protocol(&g_tern);
    mesh_protocol_attach(&harness->protocol, tern_wire_send, &harness->wire);
    (void)mesh_protocol_begin(&harness->protocol);
    return mesh_firmware_available(&harness->firmware) &&
           tern_node_greets(harness, "0.4.0-alpha.1");
}

static void tern_harness_down(struct tern_harness *harness) {
    if (harness->update_up) {
        mesh_firmware_update_shutdown(&harness->update);
    }
    if (harness->firmware_up) {
        mesh_firmware_shutdown(&harness->firmware);
    }
    if (harness->loop_up) {
        inkwell_loop_shutdown(&harness->loop);
    }
    unsetenv("MESHCLIENT_FIRMWARE_TERN_URL");
    unsetenv("MESHCLIENT_FIRMWARE_STAGING");
    https_fixture_stop(&harness->server);
    if (harness->cdn.dir[0] == '\0') {
        return;
    }
    static const char *const k_files[] = {"latest.json", TERN_IMAGE_NAME, "firmware.image"};
    char path[256];
    for (size_t i = 0; i < sizeof k_files / sizeof k_files[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", harness->cdn.dir, k_files[i]);
        (void)unlink(path);
    }
    (void)rmdir(harness->cdn.dir);
}

static bool tern_check_settles(struct tern_harness *harness) {
    for (int turn = 0; turn < 600; ++turn) {
        if (harness->firmware.state != MESH_FIRMWARE_CHECKING) {
            return true;
        }
        (void)inkwell_loop_run(&harness->loop, 10);
        mesh_firmware_tick(&harness->firmware, 0U);
    }
    return false;
}

/* Turns the loop and the job until the job reaches `state` or stops. */
static bool tern_update_reaches(struct tern_harness *harness,
                                enum mesh_firmware_update_state state) {
    for (int turn = 0; turn < 900; ++turn) {
        if (harness->update.state == state) {
            return true;
        }
        if (!mesh_firmware_update_busy(&harness->update)) {
            return false;
        }
        (void)inkwell_loop_run(&harness->loop, 10);
        harness->now_ms += 10U;
        mesh_firmware_update_tick(&harness->update, harness->now_ms);
    }
    return false;
}

struct tern_probe {
    unsigned calls;
    enum mesh_firmware_update_state state;
    enum mesh_firmware_update_error error;
};

static void tern_probe_done(void *userdata, const struct mesh_firmware_update *update) {
    struct tern_probe *const probe = (struct tern_probe *)userdata;
    probe->calls++;
    probe->state = update->state;
    probe->error = update->error;
}

/*
 * The check, for a node on a network link as much as on either bus: the image is the one for
 * its board and region, and the only thing that could stand in its way is not knowing either.
 */
MESH_TEST_CASE(firmware_tern_check_offers_the_release_over_any_link, unit) {
    struct tern_harness harness;
    const char *failure = NULL;
    if (!tern_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher and a node";
        goto cleanup;
    }
    /* TCP: a link with no bus firmware could otherwise travel on. */
    mesh_firmware_set_bus(&harness.firmware, MESH_FIRMWARE_PATH_NONE, true);
    if (mesh_firmware_check_tern(&harness.firmware, g_tern.board, g_tern.self.region,
                                 g_tern.release, 0U) != 0 ||
        harness.firmware.state != MESH_FIRMWARE_CHECKING) {
        failure = "the check should start, with one document to read";
        goto cleanup;
    }
    if (!tern_check_settles(&harness) || harness.firmware.state != MESH_FIRMWARE_AVAILABLE ||
        strcmp(harness.firmware.release.version, "0.5.0-alpha.1") != 0) {
        failure = "0.4.0-alpha.1 is behind the feed's 0.5.0-alpha.1";
        goto cleanup;
    }
    const struct mesh_firmware_board *const board = mesh_firmware_board(&harness.firmware);
    if (board == NULL || board->path != MESH_FIRMWARE_PATH_LINK ||
        strcmp(harness.firmware.release.image_name, TERN_IMAGE_NAME) != 0 ||
        harness.firmware.blocker != MESH_FIRMWARE_BLOCKER_NONE) {
        failure = "the node's own image, over the link, blocked by nothing on a network link";
        goto cleanup;
    }
    if (!mesh_firmware_answers_for_tern(&harness.firmware, "heltec-v3", "EU868", "0.4.0-alpha.1") ||
        mesh_firmware_answers_for_tern(&harness.firmware, "heltec-v3", "US915", "0.4.0-alpha.1") ||
        mesh_firmware_answers_for_tern(&harness.firmware, "heltec-v3", "EU868", "0.5.0-alpha.1") ||
        mesh_firmware_answers_for(&harness.firmware, 43U, "0.4.0-alpha.1") ||
        mesh_firmware_answers_for_meshcore(&harness.firmware, "heltec-v3", "0.4.0-alpha.1",
                                           false)) {
        failure = "the answer is this node's, in this region, on this release, and no other's";
        goto cleanup;
    }
    mesh_firmware_set_bus(&harness.firmware, MESH_FIRMWARE_PATH_NONE, false);
    if (harness.firmware.blocker != MESH_FIRMWARE_BLOCKER_NO_RADIO) {
        failure = "with the link gone there is no radio to send it to";
        goto cleanup;
    }
    mesh_firmware_set_bus(&harness.firmware, MESH_FIRMWARE_PATH_BLE, true);

    if (mesh_firmware_check_tern(&harness.firmware, "heltec-v3", "EU868", "0.5.0-alpha.1", 0U) !=
            0 ||
        !tern_check_settles(&harness) || harness.firmware.state != MESH_FIRMWARE_UP_TO_DATE) {
        failure = "a node on the feed's release is up to date";
        goto cleanup;
    }
    if (mesh_firmware_check_tern(&harness.firmware, "heltec-v3", "", "0.4.0-alpha.1", 0U) != 0 ||
        !tern_check_settles(&harness) || harness.firmware.state != MESH_FIRMWARE_AVAILABLE ||
        harness.firmware.blocker != MESH_FIRMWARE_BLOCKER_UNKNOWN_BOARD) {
        failure = "a node with no region hears of the release and is offered no image";
        goto cleanup;
    }
    setenv("MESHCLIENT_FIRMWARE_TERN_URL", "https://example.invalid/elsewhere/latest.json", 1);
    if (mesh_firmware_check_tern(&harness.firmware, "heltec-v3", "EU868", "0.4.0-alpha.1", 0U) !=
            0 ||
        !tern_check_settles(&harness) || harness.firmware.state != MESH_FIRMWARE_UP_TO_DATE) {
        failure = "no feed is no release, and nothing to update to";
        goto cleanup;
    }

cleanup:
    tern_harness_down(&harness);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

/*
 * The whole press: the image comes down and matches the feed's digest, goes to the node in
 * update frames while the node goes on answering, and the job is done once the node is back -
 * the link having dropped while it restarted - saying the new release.
 */
MESH_TEST_CASE(firmware_tern_update_goes_over_the_link_and_ends_on_the_new_release, unit) {
    struct tern_harness harness;
    struct tern_probe probe;
    memset(&probe, 0, sizeof probe);
    const char *failure = NULL;
    if (!tern_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher and a node";
        goto cleanup;
    }
    mesh_firmware_set_bus(&harness.firmware, MESH_FIRMWARE_PATH_USB, true);
    if (mesh_firmware_check_tern(&harness.firmware, g_tern.board, g_tern.self.region,
                                 g_tern.release, 0U) != 0 ||
        !tern_check_settles(&harness) || mesh_firmware_board(&harness.firmware) == NULL) {
        failure = "the check should name the image";
        goto cleanup;
    }
    struct mesh_firmware_update_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    if (mesh_firmware_update_start(&harness.update, mesh_firmware_board(&harness.firmware),
                                   &harness.firmware.release, MESH_FIRMWARE_PATH_LINK, "", &hooks,
                                   tern_probe_done, &probe) != -ENOTSUP) {
        failure = "the link path needs a link to send the image down";
        goto cleanup;
    }
    if (mesh_firmware_update_start(&harness.update, mesh_firmware_board(&harness.firmware),
                                   &harness.firmware.release, MESH_FIRMWARE_PATH_USB, "", &hooks,
                                   tern_probe_done, &probe) != -ENOTSUP) {
        failure = "and a Tern board is not written over a bus of its own";
        goto cleanup;
    }
    hooks.tern = &g_tern;
    if (mesh_firmware_update_start(&harness.update, mesh_firmware_board(&harness.firmware),
                                   &harness.firmware.release, MESH_FIRMWARE_PATH_LINK, "", &hooks,
                                   tern_probe_done, &probe) != 0) {
        failure = "the press should start";
        goto cleanup;
    }
    if (!mesh_firmware_update_holds_the_antenna(&harness.update) ||
        mesh_firmware_update_holds_the_radio(&harness.update)) {
        failure = "the download holds the antenna and never the radio";
        goto cleanup;
    }
    if (!tern_update_reaches(&harness, MESH_FIRMWARE_UPDATE_ARMING) ||
        harness.wire.last[0] != MESH_TERN_UPDATE_BEGIN) {
        failure = "once the image is down and the link synced, UPDATE_BEGIN goes";
        goto cleanup;
    }
    if (mesh_firmware_update_holds_the_radio(&harness.update) ||
        mesh_firmware_update_radio_in_loader(&harness.update)) {
        failure = "the node goes on being a node while it takes the image";
        goto cleanup;
    }
    /* The node says where to go on from, and the chunks start. */
    const struct mesh_tern_frame updating = {
        .type = MESH_TERN_UPDATING, .seq = harness.wire.last[1], .offset = 0U};
    tern_node_says(&harness, &updating);
    mesh_firmware_update_tick(&harness.update, ++harness.now_ms);
    if (harness.update.state != MESH_FIRMWARE_UPDATE_WRITING ||
        harness.wire.last[0] != MESH_TERN_UPDATE_DATA) {
        failure = "and the job is writing";
        goto cleanup;
    }
    tern_node_answers(&harness, MESH_TERN_OK);
    tern_node_answers(&harness, MESH_TERN_OK);
    const unsigned part = mesh_firmware_update_progress(&harness.update);
    if (part != (2U * MESH_TERN_UPDATE_CHUNK * 100U) / TERN_IMAGE_LEN) {
        failure = "progress is what the node holds of the image";
        goto cleanup;
    }

    /* The link drops part of the way: the job waits, and goes on after the next sync. */
    mesh_protocol_detach(&harness.protocol);
    mesh_firmware_update_tick(&harness.update, ++harness.now_ms);
    if (harness.update.state != MESH_FIRMWARE_UPDATE_WAITING || probe.calls != 0U) {
        failure = "a dropped link waits rather than failing";
        goto cleanup;
    }
    if (!tern_node_reconnects(&harness, "0.4.0-alpha.1") ||
        harness.wire.last[0] != MESH_TERN_UPDATE_BEGIN) {
        failure = "the link back, the update asks where to go on from";
        goto cleanup;
    }
    tern_node_takes_updates(&harness, 0U);
    mesh_firmware_update_tick(&harness.update, ++harness.now_ms);
    if (harness.update.state != MESH_FIRMWARE_UPDATE_RESTARTING || probe.calls != 0U ||
        harness.update.link_image != NULL) {
        failure = "UPDATE_END answered: the node restarts, and the link is done with the image";
        goto cleanup;
    }
    /* Still the old INFO on hand until the link has gone and come back. */
    mesh_firmware_update_tick(&harness.update, ++harness.now_ms);
    if (harness.update.state != MESH_FIRMWARE_UPDATE_RESTARTING) {
        failure = "the node that answered before it restarted is not the one that came back";
        goto cleanup;
    }
    mesh_protocol_detach(&harness.protocol);
    mesh_firmware_update_tick(&harness.update, ++harness.now_ms);
    if (!tern_node_reconnects(&harness, "0.5.0-alpha.1")) {
        failure = "the node should come back";
        goto cleanup;
    }
    mesh_firmware_update_tick(&harness.update, ++harness.now_ms);
    if (probe.calls != 1U || probe.state != MESH_FIRMWARE_UPDATE_DONE) {
        failure = "back on 0.5.0-alpha.1: done, once";
        goto cleanup;
    }

cleanup:
    tern_harness_down(&harness);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

/*
 * What does not end in the new release: an image that is not the feed's, which never reaches the
 * node; one the node refuses; and a node that comes back on the release it had.
 */
MESH_TEST_CASE(firmware_tern_update_refusals_leave_the_node_as_it_was, unit) {
    struct tern_harness harness;
    struct tern_probe probe;
    memset(&probe, 0, sizeof probe);
    const char *failure = NULL;
    if (!tern_harness_up(&harness)) {
        failure = "the harness should come up with a fetcher and a node";
        goto cleanup;
    }
    struct mesh_firmware_update_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.tern = &g_tern;

    /* A feed whose digest is not the image's. */
    if (!tern_write_feed(&harness, TERN_DIGEST) ||
        mesh_firmware_check_tern(&harness.firmware, "heltec-v3", "EU868", "0.4.0-alpha.1", 0U) !=
            0 ||
        !tern_check_settles(&harness) || mesh_firmware_board(&harness.firmware) == NULL) {
        failure = "the check should name the image";
        goto cleanup;
    }
    if (mesh_firmware_update_start(&harness.update, mesh_firmware_board(&harness.firmware),
                                   &harness.firmware.release, MESH_FIRMWARE_PATH_LINK, "", &hooks,
                                   tern_probe_done, &probe) != 0 ||
        tern_update_reaches(&harness, MESH_FIRMWARE_UPDATE_ARMING) || probe.calls != 1U ||
        probe.error != MESH_FIRMWARE_UPDATE_ERROR_WRONG_IMAGE || harness.wire.updates != 0U) {
        failure = "an image that is not the feed's digest is the wrong image, and never sent";
        goto cleanup;
    }

    /* The right image, refused at the end: ERROR 11, not an image this node runs. */
    char digest[INKWELL_SHA256_DIGEST_LEN * 2U + 1U];
    tern_image_digest(digest);
    probe.calls = 0U;
    if (!tern_write_feed(&harness, digest) ||
        mesh_firmware_check_tern(&harness.firmware, "heltec-v3", "EU868", "0.4.0-alpha.1", 0U) !=
            0 ||
        !tern_check_settles(&harness) ||
        mesh_firmware_update_start(&harness.update, mesh_firmware_board(&harness.firmware),
                                   &harness.firmware.release, MESH_FIRMWARE_PATH_LINK, "", &hooks,
                                   tern_probe_done, &probe) != 0 ||
        !tern_update_reaches(&harness, MESH_FIRMWARE_UPDATE_ARMING)) {
        failure = "the second press should reach the node";
        goto cleanup;
    }
    tern_node_takes_updates(&harness, MESH_TERN_ERR_NOT_AN_IMAGE);
    mesh_firmware_update_tick(&harness.update, ++harness.now_ms);
    if (probe.calls != 1U || probe.error != MESH_FIRMWARE_UPDATE_ERROR_WRONG_IMAGE ||
        harness.update.link_image != NULL) {
        failure = "ERROR 11 is the wrong image, and the copy is freed";
        goto cleanup;
    }

    /* Taken, and back on the release it had. */
    probe.calls = 0U;
    if (mesh_firmware_update_start(&harness.update, mesh_firmware_board(&harness.firmware),
                                   &harness.firmware.release, MESH_FIRMWARE_PATH_LINK, "", &hooks,
                                   tern_probe_done, &probe) != 0 ||
        !tern_update_reaches(&harness, MESH_FIRMWARE_UPDATE_ARMING)) {
        failure = "the third press should reach the node";
        goto cleanup;
    }
    tern_node_takes_updates(&harness, 0U);
    mesh_firmware_update_tick(&harness.update, ++harness.now_ms);
    mesh_protocol_detach(&harness.protocol);
    mesh_firmware_update_tick(&harness.update, ++harness.now_ms);
    if (!tern_node_reconnects(&harness, "0.4.0-alpha.1")) {
        failure = "the node should come back";
        goto cleanup;
    }
    mesh_firmware_update_tick(&harness.update, ++harness.now_ms);
    if (probe.calls != 1U || probe.state != MESH_FIRMWARE_UPDATE_FAILED ||
        probe.error != MESH_FIRMWARE_UPDATE_ERROR_HANDOVER ||
        strstr(harness.update.detail, "0.4.0-alpha.1") == NULL) {
        failure = "a node back on its old release says which, and the job failed";
        goto cleanup;
    }

cleanup:
    tern_harness_down(&harness);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

#endif /* INKWELL_HAVE_TLS */
