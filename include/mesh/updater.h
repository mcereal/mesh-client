#pragma once

/*
 * Self-update: ask GitHub for the newest release, compare it against the version this build
 * carries (mesh/version.h), and if it is newer download the release's `meshclient` asset,
 * verify it against the digest the API reported, and swap it over the running binary.
 *
 * How it talks to the network. There is no TLS in this process - the release build is a static
 * musl binary whose only dependency is libdbus - so the updater forks the device's own curl
 * (falling back to wget) and reads its stdout through the event loop, the same shape
 * src/ui/backends/minui.c already uses for minui-list. No threads, one child at a time, and
 * every step is a state the About screen can name.
 *
 * What makes downloading an executable safe here is not the transport but the digest: the
 * release metadata is read from api.github.com, the asset URL is checked against that release's
 * own asset list, and the downloaded bytes must hash to the digest that metadata carried
 * before anything is moved into place. A redirect to a CDN (which is what GitHub actually
 * serves) is therefore fine, and a truncated or substituted download fails closed.
 *
 * The install is a rename() within one directory, so it is atomic: either the old binary or
 * the new one is at the path, never a partial file. Linux keeps the running image alive off
 * its inode, so the swap happens under a running client without disturbing it - which is why
 * the last state is READY ("relaunch to run it") rather than a restart the app performs
 * itself.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

/* Where an update attempt has got to. The About screen renders this directly. */
enum mesh_update_state {
    MESH_UPDATE_IDLE = 0, /* never checked this run */
    MESH_UPDATE_CHECKING, /* asking GitHub for the latest release */
    MESH_UPDATE_UP_TO_DATE,
    MESH_UPDATE_AVAILABLE, /* a newer release exists; `latest` and the asset are held */
    MESH_UPDATE_DOWNLOADING,
    MESH_UPDATE_VERIFYING, /* hashing what was downloaded */
    MESH_UPDATE_READY,     /* installed; relaunch to run it */
    MESH_UPDATE_FAILED,    /* `message` says why */
    MESH_UPDATE_STATE_COUNT,
};

#define MESH_UPDATE_VERSION_MAX 32U
#define MESH_UPDATE_URL_MAX 256U
#define MESH_UPDATE_MESSAGE_MAX 96U
#define MESH_UPDATE_PATH_MAX 256U
/* Release JSON runs to a few KB; anything past this is a response we would not trust anyway. */
#define MESH_UPDATE_RESPONSE_MAX 65536U

struct mesh_updater {
    enum mesh_update_state state;
    /* One line for the UI: the failure reason, or what is happening. Always NUL-terminated. */
    char message[MESH_UPDATE_MESSAGE_MAX];
    /* The newest release's tag, without its leading 'v'. Empty until a check succeeds. */
    char latest[MESH_UPDATE_VERSION_MAX];
    char asset_url[MESH_UPDATE_URL_MAX];
    /* Lower-case hex sha256 the download must hash to. Empty when the release carried none,
       which is refused rather than installed unverified. */
    char asset_sha256[65];
    uint64_t asset_size;
    /* The binary being replaced (/proc/self/exe) and the temporary name next to it. */
    char install_path[MESH_UPDATE_PATH_MAX];
    /* Room for install_path plus the ".update" suffix, so staging can never truncate. */
    char staged_path[MESH_UPDATE_PATH_MAX + 16U];
    /* "curl" or "wget"; empty when the device has neither and updates are unavailable. */
    const char *fetcher;
    /* CA bundle handed to the fetcher, or empty to leave it on its own defaults. The Brick has
       no system CA store at all, so without one every HTTPS fetch fails; the pak ships a bundle
       in certs/ and this is wherever it was found. See updater_resolve_ca_bundle(). */
    char ca_bundle[MESH_UPDATE_PATH_MAX];

    struct mesh_event_loop *loop;
    /* The running child, or -1. Only ever one: the states are strictly sequential. */
    pid_t child;
    int child_fd;
    uint64_t child_deadline_ms;
    /* Captured child stdout, for the check. The download writes straight to a file. */
    char *response;
    size_t response_len;
    /* Bumped whenever anything above changes, so app.c can publish without diffing. */
    uint32_t revision;
};

/* `loop` may be NULL, in which case the updater reports itself unavailable. Probes for a
   fetcher on PATH. Returns 0, or -errno. */
int mesh_updater_init(struct mesh_updater *updater, struct mesh_event_loop *loop);
void mesh_updater_shutdown(struct mesh_updater *updater);

/* True when a fetcher was found and the running binary's path is known, i.e. when check and
   install can do anything at all. */
bool mesh_updater_available(const struct mesh_updater *updater);

/* Starts a check. No-op while a child is running. Returns 0, or -errno. */
int mesh_updater_check(struct mesh_updater *updater, uint64_t now_ms);
/* Downloads and installs the release the last check found. Only valid in AVAILABLE. */
int mesh_updater_install(struct mesh_updater *updater, uint64_t now_ms);

/* Enforces the per-step timeout and reaps a finished child. Call every loop turn. */
void mesh_updater_tick(struct mesh_updater *updater, uint64_t now_ms);

const char *mesh_update_state_name(enum mesh_update_state state);

/*
 * Parses a GitHub "releases/latest" response for the tag and the asset named `asset_name`.
 * Split out from the fetch so it can be tested against captured JSON, and written to be
 * indifferent to key order and to unknown keys.
 *
 * The URL is only accepted when it is an https URL under the release-download path of the
 * repository that was asked about, so a response that has been tampered with cannot point the
 * download somewhere else. Returns true when both a tag and an asset URL were found.
 */
bool mesh_updater_parse_release(const char *json, const char *repo, const char *asset_name,
                                char *out_tag, size_t out_tag_len, char *out_url,
                                size_t out_url_len, char *out_sha256, size_t out_sha256_len,
                                uint64_t *out_size);

/* owner/repo the updater asks about, and the release asset it looks for. Both are overridable
   through the environment (MESHCLIENT_UPDATE_REPO, MESHCLIENT_UPDATE_ASSET) so a fork or a
   test can point them elsewhere. */
const char *mesh_updater_repo(void);
const char *mesh_updater_asset_name(void);

#ifdef __cplusplus
}
#endif
