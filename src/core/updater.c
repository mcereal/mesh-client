#define _POSIX_C_SOURCE 200809L

#include "mesh/core/updater.h"

#include "mesh/i18n/strings.h"

#include "mesh/core/event_loop.h"
#include "mesh/core/version.h"
#include "mesh/utils/env.h"
#include "mesh/utils/log.h"
#include "mesh/utils/sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MESHCLIENT_UPDATE_REPO
#define MESHCLIENT_UPDATE_REPO "mcereal/mesh-client"
#endif
#ifndef MESHCLIENT_UPDATE_ASSET
#define MESHCLIENT_UPDATE_ASSET "meshclient-tg5040-aarch64"
#endif

/* A release check is a few KB over HTTPS; a download is ~1 MB over whatever WiFi the Brick
   has. Both are generous, and both exist so a stalled child cannot wedge the About screen. */
#define MESH_UPDATE_CHECK_TIMEOUT_MS 30000U
#define MESH_UPDATE_DOWNLOAD_TIMEOUT_MS 300000U
/* Refuse an asset that is not plausibly our binary before spending the download on it. */
#define MESH_UPDATE_MAX_ASSET_BYTES (32U * 1024U * 1024U)

const char *mesh_updater_repo(void) {
    const char *from_env = getenv("MESHCLIENT_UPDATE_REPO");
    return (from_env != NULL && from_env[0] != '\0') ? from_env : MESHCLIENT_UPDATE_REPO;
}

const char *mesh_updater_asset_name(void) {
    const char *from_env = getenv("MESHCLIENT_UPDATE_ASSET");
    return (from_env != NULL && from_env[0] != '\0') ? from_env : MESHCLIENT_UPDATE_ASSET;
}

const char *mesh_update_state_name(enum mesh_update_state state) {
    switch (state) {
    case MESH_UPDATE_IDLE:
        return mesh_str(MESH_STR_UPDATE_STATE_IDLE);
    case MESH_UPDATE_CHECKING:
        return mesh_str(MESH_STR_UPDATE_STATE_CHECKING);
    case MESH_UPDATE_UP_TO_DATE:
        return mesh_str(MESH_STR_UPDATE_STATE_UP_TO_DATE);
    case MESH_UPDATE_AVAILABLE:
        return mesh_str(MESH_STR_UPDATE_STATE_AVAILABLE);
    case MESH_UPDATE_DOWNLOADING:
        return mesh_str(MESH_STR_UPDATE_STATE_DOWNLOADING);
    case MESH_UPDATE_VERIFYING:
        return mesh_str(MESH_STR_UPDATE_STATE_VERIFYING);
    case MESH_UPDATE_READY:
        return mesh_str(MESH_STR_UPDATE_STATE_READY);
    case MESH_UPDATE_FAILED:
        return mesh_str(MESH_STR_UPDATE_STATE_FAILED);
    default:
        return mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
    }
}

const char *mesh_update_channel_name(enum mesh_update_channel channel) {
    switch (channel) {
    case MESH_UPDATE_CHANNEL_STABLE:
        return mesh_str(MESH_STR_UPDATE_CHANNEL_STABLE);
    case MESH_UPDATE_CHANNEL_PRERELEASE:
        return mesh_str(MESH_STR_UPDATE_CHANNEL_PRERELEASE);
    case MESH_UPDATE_CHANNEL_DEFAULT:
        /* Say what the inference resolved to, or the row would read as a shrug - and ask the
           function that does the resolving rather than repeating its rule. Spelling the rule
           out a second time here is how the label came to say "prerelease" on every -dev build
           while the check went to the stable endpoint: a `-dev` suffix makes
           mesh_version_is_prerelease() true on its own, but the inference also requires the
           build to be a release. NULL is the updater on the default channel, by definition. */
        return mesh_str(mesh_updater_effective_channel(NULL) == MESH_UPDATE_CHANNEL_PRERELEASE
                            ? MESH_STR_UPDATE_CHANNEL_AUTO_PRE
                            : MESH_STR_UPDATE_CHANNEL_AUTO_STABLE);
    default:
        return mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
    }
}

enum mesh_update_channel mesh_updater_effective_channel(const struct mesh_updater *updater) {
    const enum mesh_update_channel channel =
        updater != NULL ? updater->channel : MESH_UPDATE_CHANNEL_DEFAULT;
    if (channel == MESH_UPDATE_CHANNEL_STABLE || channel == MESH_UPDATE_CHANNEL_PRERELEASE) {
        return channel;
    }
    /* A build off beta or rc follows prereleases; anything else follows stable. This is what
       the updater did before the channel was a setting, so DEFAULT changes nothing. */
    return (mesh_version_is_release() && mesh_version_is_prerelease())
               ? MESH_UPDATE_CHANNEL_PRERELEASE
               : MESH_UPDATE_CHANNEL_STABLE;
}

bool mesh_updater_can_install(const struct mesh_updater *updater) {
    return updater != NULL && (updater->allow_dev || mesh_version_is_release());
}

bool mesh_updater_holds_the_radio(const struct mesh_updater *updater) {
    /* VERIFYING is local hashing and wants no antenna of its own, but it sits between the
       download and the install with a relaunch on the far side; bringing a link up for the
       second or two it lasts would only spend the sync it could not finish. */
    return updater != NULL &&
           (updater->state == MESH_UPDATE_DOWNLOADING || updater->state == MESH_UPDATE_VERIFYING);
}

/* Drops the release the last check found. Anything that could make it stale - a new check, a
   channel change - goes through here so an install can never be handed an asset from a
   question we are no longer asking. */
static void updater_forget_release(struct mesh_updater *updater) {
    updater->latest[0] = '\0';
    updater->asset_url[0] = '\0';
    updater->asset_sha256[0] = '\0';
    updater->asset_size = 0U;
}

/* Drops whatever the last check concluded, because the question it answered has changed. An
   updater still sitting at IDLE keeps init()'s `message` - "no curl or wget on this device" is
   a fact about the install, not a stale result. */
static void updater_invalidate_check(struct mesh_updater *updater, const char *why);

static void updater_set(struct mesh_updater *updater, enum mesh_update_state state,
                        const char *message) {
    /* A byte count belongs to the download it was counted for. Leaving it behind would show the
       next step - or the next attempt - starting from wherever the last one stopped, which for
       a failed download is a bar that reports the failure as progress. */
    if (state != MESH_UPDATE_DOWNLOADING) {
        updater->downloaded = 0U;
    }
    updater->state = state;
    snprintf(updater->message, sizeof updater->message, "%s", message != NULL ? message : "");
    updater->revision++;
}

static void updater_invalidate_check(struct mesh_updater *updater, const char *why) {
    updater_forget_release(updater);
    if (updater->state != MESH_UPDATE_IDLE) {
        updater_set(updater, MESH_UPDATE_IDLE, why);
    } else {
        updater->revision++;
    }
}

/* ---- release JSON ---------------------------------------------------------------------- */

/*
 * Just enough JSON to read a GitHub release. Not a parser: a scanner that walks the text
 * looking for `"key":` at any depth and reads the string or number that follows. That is
 * sufficient because the shape being read is fixed and shallow (a tag at the top, an array of
 * assets each with a name, a URL, a size and a digest) and because nothing here is trusted on
 * the strength of having been found - the URL is checked against the repository it must belong
 * to, and the bytes it yields are checked against the digest.
 */

static const char *skip_space(const char *cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
        cursor++;
    }
    return cursor;
}

/* Copies a JSON string body into `out`, resolving the escapes GitHub can actually emit.
   Returns the character after the closing quote, or NULL. `cursor` points at the opening
   quote. A string that would overflow `out` fails rather than truncating: a half-copied URL
   must never be treated as a URL. */
static const char *read_string(const char *cursor, char *out, size_t out_len) {
    if (*cursor != '"') {
        return NULL;
    }
    cursor++;
    size_t pos = 0U;
    while (*cursor != '\0' && *cursor != '"') {
        char decoded;
        if (*cursor == '\\') {
            cursor++;
            switch (*cursor) {
            case '"':
            case '\\':
            case '/':
                decoded = *cursor;
                break;
            case 'n':
                decoded = '\n';
                break;
            case 't':
                decoded = '\t';
                break;
            case 'r':
                decoded = '\r';
                break;
            case 'b':
                decoded = '\b';
                break;
            case 'f':
                decoded = '\f';
                break;
            case 'u': {
                /* Only the ASCII range matters for the fields read here; anything else becomes
                   '?' so a \u escape cannot smuggle bytes past the checks below. */
                if (strlen(cursor) < 5U) {
                    return NULL;
                }
                unsigned code = 0U;
                for (unsigned i = 1U; i <= 4U; ++i) {
                    const char c = cursor[i];
                    unsigned nibble;
                    if (c >= '0' && c <= '9') {
                        nibble = (unsigned)(c - '0');
                    } else if (c >= 'a' && c <= 'f') {
                        nibble = (unsigned)(c - 'a') + 10U;
                    } else if (c >= 'A' && c <= 'F') {
                        nibble = (unsigned)(c - 'A') + 10U;
                    } else {
                        return NULL;
                    }
                    code = (code << 4) | nibble;
                }
                cursor += 4;
                decoded = (code >= 0x20U && code < 0x7FU) ? (char)code : '?';
                break;
            }
            default:
                return NULL;
            }
            cursor++;
        } else {
            decoded = *cursor++;
        }
        if (pos + 1U >= out_len) {
            return NULL;
        }
        out[pos++] = decoded;
    }
    if (*cursor != '"') {
        return NULL;
    }
    out[pos] = '\0';
    return cursor + 1;
}

/* The position just after `"key":` at or after `from`, or NULL. Only matches a key, i.e. a
   quoted name followed by a colon, so a value that happens to contain the text is skipped. */
static const char *find_key(const char *from, const char *key) {
    const size_t key_len = strlen(key);
    for (const char *cursor = from; (cursor = strchr(cursor, '"')) != NULL; cursor++) {
        if (strncmp(cursor + 1, key, key_len) != 0 || cursor[1U + key_len] != '"') {
            continue;
        }
        const char *after = skip_space(cursor + key_len + 2U);
        if (*after == ':') {
            return skip_space(after + 1);
        }
    }
    return NULL;
}

/* Reads the string value of `key` at or after `from`. */
static bool read_key_string(const char *from, const char *key, char *out, size_t out_len) {
    const char *value = find_key(from, key);
    return value != NULL && read_string(value, out, out_len) != NULL;
}

static bool read_key_number(const char *from, const char *key, uint64_t *out) {
    const char *value = find_key(from, key);
    if (value == NULL || *value < '0' || *value > '9') {
        return false;
    }
    uint64_t parsed = 0U;
    while (*value >= '0' && *value <= '9') {
        if (parsed > (UINT64_MAX - 9U) / 10U) {
            return false;
        }
        parsed = parsed * 10U + (uint64_t)(*value - '0');
        value++;
    }
    *out = parsed;
    return true;
}

/*
 * The asset URL must be exactly where this repository's release downloads live. That is what
 * stops a mangled or hostile response from aiming the download at another host - the digest
 * check that follows only proves the bytes match what the metadata said, so the metadata has
 * to be about the right thing.
 */
static bool url_belongs_to_repo(const char *url, const char *repo) {
    char prefix[MESH_UPDATE_URL_MAX];
    const int written =
        snprintf(prefix, sizeof prefix, "https://github.com/%s/releases/download/", repo);
    if (written <= 0 || (size_t)written >= sizeof prefix) {
        return false;
    }
    const size_t prefix_len = strlen(prefix);
    if (strncmp(url, prefix, prefix_len) != 0) {
        return false;
    }
    /* Nothing after the prefix may climb back out of it or start a new authority. */
    const char *rest = url + prefix_len;
    return rest[0] != '\0' && strstr(rest, "..") == NULL && strstr(rest, "//") == NULL;
}

/* GitHub reports asset digests as "sha256:<64 hex>"; keep only well-formed ones. */
static bool digest_to_hex(const char *digest, char *out, size_t out_len) {
    static const char k_prefix[] = "sha256:";
    if (strncmp(digest, k_prefix, sizeof k_prefix - 1U) != 0) {
        return false;
    }
    const char *hex = digest + sizeof k_prefix - 1U;
    if (strlen(hex) != 64U || out_len < 65U) {
        return false;
    }
    for (size_t i = 0; i < 64U; ++i) {
        const char c = hex[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) {
            return false;
        }
        out[i] = (char)((c >= 'A' && c <= 'F') ? c - 'A' + 'a' : c);
    }
    out[64] = '\0';
    return true;
}

bool mesh_updater_parse_release(const char *json, const char *repo, const char *asset_name,
                                char *out_tag, size_t out_tag_len, char *out_url,
                                size_t out_url_len, char *out_sha256, size_t out_sha256_len,
                                uint64_t *out_size) {
    if (json == NULL || repo == NULL || asset_name == NULL || out_tag == NULL || out_url == NULL) {
        return false;
    }
    out_tag[0] = '\0';
    out_url[0] = '\0';
    if (out_sha256 != NULL && out_sha256_len > 0U) {
        out_sha256[0] = '\0';
    }
    if (out_size != NULL) {
        *out_size = 0U;
    }

    char tag[MESH_UPDATE_VERSION_MAX];
    if (!read_key_string(json, "tag_name", tag, sizeof tag) || tag[0] == '\0') {
        return false;
    }
    /* Tags carry a leading 'v'; the version this build reports does not. */
    const char *trimmed = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
    if (snprintf(out_tag, out_tag_len, "%s", trimmed) < 0) {
        return false;
    }

    /* Walk the assets by their `name` keys: the entry whose name matches is the one whose
       following url/size/digest belong to it. */
    const char *cursor = find_key(json, "assets");
    if (cursor == NULL) {
        return false;
    }
    while ((cursor = find_key(cursor, "name")) != NULL) {
        char name[128];
        const char *after = read_string(cursor, name, sizeof name);
        if (after == NULL) {
            cursor++;
            continue;
        }
        cursor = after;
        if (strcmp(name, asset_name) != 0) {
            continue;
        }
        char url[MESH_UPDATE_URL_MAX];
        if (!read_key_string(after, "browser_download_url", url, sizeof url) ||
            !url_belongs_to_repo(url, repo)) {
            return false;
        }
        if (snprintf(out_url, out_url_len, "%s", url) < 0) {
            return false;
        }
        uint64_t size = 0U;
        if (out_size != NULL && read_key_number(after, "size", &size)) {
            *out_size = size;
        }
        char digest[128];
        if (out_sha256 != NULL && read_key_string(after, "digest", digest, sizeof digest)) {
            (void)digest_to_hex(digest, out_sha256, out_sha256_len);
        }
        return true;
    }
    return false;
}

/* ---- the pak around us ------------------------------------------------------------------ */

/*
 * Find a file that sits at the root of the pak we were installed into.
 *
 * The binary lives at <pak>/bin/shared/meshclient, so the root is three directories up; a build
 * running from anywhere else simply finds nothing, which is what every caller here wants. Used
 * for both the CA bundle we ship and the pak.json the store reads.
 */
static bool updater_pak_file(const char *install_path, const char *relative, char *out,
                             size_t out_len) {
    if (install_path == NULL || install_path[0] == '\0') {
        return false;
    }
    char path[MESH_UPDATE_PATH_MAX + 16U];
    if ((size_t)snprintf(path, sizeof path, "%s", install_path) >= sizeof path) {
        return false;
    }
    /* <pak>/bin/shared/meshclient: the binary's own directory, then bin/, then the pak root. */
    for (int level = 0; level < 3; ++level) {
        char *slash = strrchr(path, '/');
        if (slash == NULL || slash == path) {
            return false;
        }
        *slash = '\0';
        char candidate[sizeof path + 64U];
        if ((size_t)snprintf(candidate, sizeof candidate, "%s/%s", path, relative) >=
            sizeof candidate) {
            continue;
        }
        if (access(candidate, R_OK) == 0) {
            return (size_t)snprintf(out, out_len, "%s", candidate) < out_len;
        }
    }
    return false;
}

/*
 * Point the fetcher at the CA bundle this pak ships.
 *
 * The Brick has no system CA store - no /etc/ssl at all - so without one every HTTPS request
 * fails; mesh_fetch_resolve_ca_bundle() explains why `--insecure` is not the way out and what
 * it falls back to. Only the pak half is ours: where the bundle sits is a fact about how this
 * binary was installed, and the fetcher has no business knowing it.
 */
static void updater_resolve_ca_bundle(struct mesh_updater *updater) {
    char shipped[MESH_UPDATE_PATH_MAX];
    if (!updater_pak_file(updater->install_path, "certs/certificates.crt", shipped,
                          sizeof shipped)) {
        shipped[0] = '\0';
    }
    mesh_fetch_resolve_ca_bundle(&updater->fetch, shipped);
}

/*
 * The one line the About screen shows when a fetch did not come back with a document.
 *
 * curl's 60 is specifically "peer certificate cannot be authenticated", which on a device with
 * no CA store is the only thing that will ever happen and which "exit 60" tells nobody how to
 * fix. The bundle ships in the pak and not through self-update, so the answer really is to
 * reinstall the pak.
 *
 * `what` names the catalog entry for the phase that failed - a check or a download - so the
 * sentence is one string rather than a verb glued onto a template.
 */
static void updater_fetch_failed(struct mesh_updater *updater,
                                 const struct mesh_fetch_result *result, enum mesh_str_id what) {
    char message[MESH_UPDATE_MESSAGE_MAX];
    switch (result->outcome) {
    case MESH_FETCH_TOO_LARGE:
        snprintf(message, sizeof message, "%s", mesh_str(MESH_STR_UPDATE_RESPONSE_TOO_LARGE));
        break;
    case MESH_FETCH_READ_FAILED:
        snprintf(message, sizeof message, "%s", mesh_str(MESH_STR_UPDATE_READ_FAILED));
        break;
    case MESH_FETCH_TIMED_OUT:
        mesh_log_warn("update", "%s timed out in state %s", mesh_fetch_tool(&updater->fetch),
                      mesh_update_state_name(updater->state));
        snprintf(message, sizeof message, "%s", mesh_str(MESH_STR_UPDATE_TIMED_OUT));
        break;
    case MESH_FETCH_EXITED:
    case MESH_FETCH_OK:
    case MESH_FETCH_OUTCOME_COUNT:
    default:
        if (strcmp(mesh_fetch_tool(&updater->fetch), "curl") == 0 && result->status == 60) {
            snprintf(message, sizeof message, "%s",
                     mesh_str(updater->fetch.ca_bundle[0] != '\0' ? MESH_STR_UPDATE_TLS_UNVERIFIED
                                                                  : MESH_STR_UPDATE_NO_CA_BUNDLE));
        } else {
            mesh_str_format(message, sizeof message, what, mesh_fetch_tool(&updater->fetch),
                            result->status);
        }
        break;
    }
    updater_set(updater, MESH_UPDATE_FAILED, message);
}

/* ---- steps ------------------------------------------------------------------------------ */

int mesh_updater_init(struct mesh_updater *updater, struct mesh_event_loop *loop) {
    if (updater == NULL) {
        return -EINVAL;
    }
    memset(updater, 0, sizeof *updater);
    updater->state = MESH_UPDATE_IDLE;
    const int ready = mesh_fetch_init(&updater->fetch, loop);
    if (ready != 0) {
        return ready;
    }

    /* The binary to replace. Without this there is nothing to install over, so the About
       screen offers only the version rather than a broken update row. */
    const ssize_t len =
        readlink("/proc/self/exe", updater->install_path, sizeof updater->install_path - 1U);
    if (len > 0) {
        updater->install_path[len] = '\0';
        snprintf(updater->staged_path, sizeof updater->staged_path, "%s.update",
                 updater->install_path);
    } else {
        updater->install_path[0] = '\0';
    }

    /* Needs install_path, so it has to come after the readlink above. */
    updater_resolve_ca_bundle(updater);

    updater->allow_dev_from_env =
        mesh_env_bool("MESHCLIENT_UPDATE_ALLOW_DEV", "dev updates", false);
    updater->allow_dev = updater->allow_dev_from_env;

    if (updater->fetch.tool == NULL) {
        snprintf(updater->message, sizeof updater->message, "%s",
                 mesh_str(MESH_STR_UPDATE_NO_FETCHER));
    } else if (!mesh_version_is_release() && !updater->allow_dev) {
        /* Say the consequence, not just the fact: the old wording ("Development build") sat
           next to a check that would go on to name a newer release it had no intention of
           installing, which reads as a broken install button rather than a deliberate guard. */
        snprintf(updater->message, sizeof updater->message, "%s",
                 mesh_str(MESH_STR_UPDATE_DEV_DISABLED));
    } else if (!mesh_version_is_release()) {
        snprintf(updater->message, sizeof updater->message, "%s",
                 mesh_str(MESH_STR_UPDATE_DEV_ENABLED));
    }
    mesh_log_info(
        "update",
        "Updater ready: fetcher=%s binary=%s version=%s channel=%s allow_dev=%s "
        "cacert=%s",
        mesh_fetch_tool(&updater->fetch),
        updater->install_path[0] != '\0' ? updater->install_path : "unknown", mesh_version_string(),
        mesh_update_channel_name(mesh_updater_effective_channel(updater)),
        updater->allow_dev ? "yes" : "no",
        updater->fetch.ca_bundle[0] != '\0' ? updater->fetch.ca_bundle : "(fetcher default)");
    return 0;
}

void mesh_updater_shutdown(struct mesh_updater *updater) {
    if (updater == NULL) {
        return;
    }
    mesh_fetch_shutdown(&updater->fetch);
    /* Half a download left behind would otherwise sit next to the binary until the next run. */
    if (updater->state == MESH_UPDATE_DOWNLOADING && updater->staged_path[0] != '\0') {
        (void)unlink(updater->staged_path);
    }
}

bool mesh_updater_available(const struct mesh_updater *updater) {
    return updater != NULL && mesh_fetch_available(&updater->fetch) &&
           updater->install_path[0] != '\0';
}

bool mesh_updater_set_channel(struct mesh_updater *updater, enum mesh_update_channel channel) {
    if (updater == NULL || channel >= MESH_UPDATE_CHANNEL_COUNT || updater->channel == channel) {
        return false;
    }
    if (mesh_fetch_busy(&updater->fetch)) {
        return false; /* mid-check or mid-download: the asset in flight belongs to the old one */
    }
    updater->channel = channel;
    updater_invalidate_check(updater, mesh_str(MESH_STR_UPDATE_CHANNEL_CHANGED));
    mesh_log_info("update", "Update channel set to %s",
                  mesh_update_channel_name(mesh_updater_effective_channel(updater)));
    return true;
}

bool mesh_updater_set_allow_dev(struct mesh_updater *updater, bool allow) {
    if (updater == NULL || updater->allow_dev == allow) {
        return false;
    }
    if (mesh_fetch_busy(&updater->fetch)) {
        return false; /* mid-check or mid-download; let it finish rather than move the goalposts */
    }
    updater->allow_dev = allow;
    updater_invalidate_check(updater, mesh_str(MESH_STR_UPDATE_SETTING_CHANGED));
    /* init() put its reason in `message` when the updater was left idle, and that reason has
       just stopped being true either way. */
    if (updater->state == MESH_UPDATE_IDLE && !mesh_version_is_release()) {
        snprintf(updater->message, sizeof updater->message, "%s",
                 mesh_str(allow ? MESH_STR_UPDATE_DEV_ENABLED : MESH_STR_UPDATE_DEV_DISABLED));
    }
    mesh_log_info("update", "Dev updates %s", allow ? "enabled" : "disabled");
    return true;
}

/* The fetcher calls these once each, from the loop, when its child is gone. */
static void updater_on_check_done(void *userdata, const struct mesh_fetch_result *result);
static void updater_on_download_done(void *userdata, const struct mesh_fetch_result *result);

int mesh_updater_check(struct mesh_updater *updater, uint64_t now_ms) {
    if (updater == NULL) {
        return -EINVAL;
    }
    if (!mesh_updater_available(updater)) {
        return -ENOTSUP;
    }
    if (mesh_fetch_busy(&updater->fetch)) {
        return -EBUSY;
    }
    updater_forget_release(updater);

    /*
     * Which question to ask is the channel setting (mesh_update_channel). `releases/latest`
     * deliberately skips prereleases, so a client on the stable channel is never shown a beta;
     * `releases?per_page=1` is the newest release of any kind - and capping it at one keeps the
     * reply a single release object, so the scanner below cannot pair one release's tag with
     * another's asset.
     */
    char url[MESH_UPDATE_URL_MAX];
    if (mesh_updater_effective_channel(updater) == MESH_UPDATE_CHANNEL_PRERELEASE) {
        snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases?per_page=1",
                 mesh_updater_repo());
    } else {
        snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest",
                 mesh_updater_repo());
    }
    char agent_header[80];
    snprintf(agent_header, sizeof agent_header, "User-Agent: meshclient/%s", mesh_version_string());

    const struct mesh_fetch_request request = {
        .url = url,
        .headers = {"Accept: application/vnd.github+json", agent_header},
        .timeout_ms = MESH_UPDATE_CHECK_TIMEOUT_MS,
        .response_max = MESH_UPDATE_RESPONSE_MAX,
        .on_done = updater_on_check_done,
        .userdata = updater,
    };
    const int result = mesh_fetch_start(&updater->fetch, &request, now_ms);
    if (result != 0) {
        updater_set(updater, MESH_UPDATE_FAILED, mesh_str(MESH_STR_UPDATE_START_FAILED));
        return result;
    }
    updater_set(updater, MESH_UPDATE_CHECKING, mesh_str(MESH_STR_UPDATE_CHECKING));
    return 0;
}

static void updater_on_check_done(void *userdata, const struct mesh_fetch_result *result) {
    struct mesh_updater *updater = (struct mesh_updater *)userdata;
    if (updater == NULL || updater->state != MESH_UPDATE_CHECKING) {
        return;
    }
    if (result->outcome != MESH_FETCH_OK) {
        updater_fetch_failed(updater, result, MESH_STR_UPDATE_CHECK_EXIT);
        return;
    }
    if (result->body == NULL) {
        updater_set(updater, MESH_UPDATE_FAILED, mesh_str(MESH_STR_UPDATE_EMPTY_REPLY));
        return;
    }

    char tag[MESH_UPDATE_VERSION_MAX];
    char url[MESH_UPDATE_URL_MAX];
    char sha256[65];
    uint64_t size = 0U;
    if (!mesh_updater_parse_release(result->body, mesh_updater_repo(), mesh_updater_asset_name(),
                                    tag, sizeof tag, url, sizeof url, sha256, sizeof sha256,
                                    &size)) {
        updater_set(updater, MESH_UPDATE_FAILED, mesh_str(MESH_STR_UPDATE_NO_ASSET));
        return;
    }

    snprintf(updater->latest, sizeof updater->latest, "%s", tag);
    snprintf(updater->asset_url, sizeof updater->asset_url, "%s", url);
    snprintf(updater->asset_sha256, sizeof updater->asset_sha256, "%s", sha256);
    updater->asset_size = size;
    mesh_log_info("update", "Latest release %s (running %s), asset %llu bytes", tag,
                  mesh_version_string(), (unsigned long long)size);

    if (!mesh_updater_can_install(updater)) {
        /* A check on a dev build is still useful - it says what is out there - but it must not
           read as an offer. Naming the release and the reason in one line is what stops the
           About screen from looking like an install button that does nothing. */
        char message[MESH_UPDATE_MESSAGE_MAX];
        mesh_str_format(message, sizeof message, MESH_STR_UPDATE_LATEST_DEV, tag);
        updater_set(updater, MESH_UPDATE_UP_TO_DATE, message);
        return;
    }
    /*
     * A release build compares against the release it claims to be. A build running under
     * MESHCLIENT_UPDATE_ALLOW_DEV compares its own "<version>-dev" string, which SemVer sorts
     * below the release of the same number - so it is offered exactly the release its working
     * tree is based on, which is the one worth exercising the download path against.
     */
    const bool newer = mesh_version_is_release()
                           ? mesh_version_is_newer_than_running(tag)
                           : mesh_version_compare(tag, mesh_version_string()) > 0;
    if (!newer) {
        updater_set(updater, MESH_UPDATE_UP_TO_DATE, mesh_str(MESH_STR_UPDATE_LATEST_RUNNING));
        return;
    }
    if (updater->asset_sha256[0] == '\0') {
        /* Without a digest there is no way to tell a good download from a bad one, and this
           installs an executable. Refuse rather than trust the transport alone. */
        updater_set(updater, MESH_UPDATE_FAILED, mesh_str(MESH_STR_UPDATE_NO_CHECKSUM));
        return;
    }
    if (size == 0U || size > MESH_UPDATE_MAX_ASSET_BYTES) {
        updater_set(updater, MESH_UPDATE_FAILED, mesh_str(MESH_STR_UPDATE_BAD_SIZE));
        return;
    }

    char message[MESH_UPDATE_MESSAGE_MAX];
    mesh_str_format(message, sizeof message, MESH_STR_UPDATE_AVAILABLE, tag, mesh_version_string());
    updater_set(updater, MESH_UPDATE_AVAILABLE, message);
}

int mesh_updater_install(struct mesh_updater *updater, uint64_t now_ms) {
    if (updater == NULL) {
        return -EINVAL;
    }
    if (!mesh_updater_available(updater)) {
        return -ENOTSUP;
    }
    if (mesh_fetch_busy(&updater->fetch)) {
        return -EBUSY;
    }
    if (updater->state != MESH_UPDATE_AVAILABLE || updater->asset_url[0] == '\0' ||
        updater->asset_sha256[0] == '\0') {
        return -EINVAL;
    }
    (void)unlink(updater->staged_path);
    /* The staged file is the byte counter, so the unlink above is also the reset - but say it
       here too, because a failed attempt whose file could not be removed would otherwise start
       the next one at 100%. */
    updater->downloaded = 0U;

    const struct mesh_fetch_request request = {
        .url = updater->asset_url,
        .output_path = updater->staged_path,
        .timeout_ms = MESH_UPDATE_DOWNLOAD_TIMEOUT_MS,
        .on_done = updater_on_download_done,
        .userdata = updater,
    };
    const int result = mesh_fetch_start(&updater->fetch, &request, now_ms);
    if (result != 0) {
        updater_set(updater, MESH_UPDATE_FAILED, mesh_str(MESH_STR_UPDATE_DOWNLOAD_START_FAIL));
        return result;
    }
    char message[MESH_UPDATE_MESSAGE_MAX];
    mesh_str_format(message, sizeof message, MESH_STR_UPDATE_DOWNLOADING, updater->latest);
    updater_set(updater, MESH_UPDATE_DOWNLOADING, message);
    return 0;
}

/* Room for a pak.json: ours is a few hundred bytes, and anything larger than this is not one. */
#define MESH_UPDATE_PAK_JSON_MAX 16384U

/*
 * Rewrite the `version` in the pak.json next to the binary we just installed.
 *
 * The Pak Store decides whether an installed pak is out of date by reading that field, and a
 * self-update replaces only the binary - so without this the store would keep offering an
 * update the device already has. The file sits at the pak root and the binary at
 * <pak>/bin/shared/meshclient, hence the walk up; a build running from somewhere else simply
 * finds nothing.
 *
 * Best effort by design: the install has already succeeded by the time this runs, so a pak.json
 * that is missing, oversized or shaped differently is logged and left alone rather than turned
 * into a failed update.
 */
static void updater_stamp_pak_json(const struct mesh_updater *updater) {
    if (updater->install_path[0] == '\0' || updater->latest[0] == '\0') {
        return;
    }

    char json_path[MESH_UPDATE_PATH_MAX + 32U];
    if (!updater_pak_file(updater->install_path, "pak.json", json_path, sizeof json_path)) {
        return;
    }
    if (access(json_path, W_OK) != 0) {
        mesh_log_warn("update", "%s is not writable; leaving its version alone", json_path);
        return;
    }
    char temp_path[sizeof json_path + 8U];
    snprintf(temp_path, sizeof temp_path, "%s.new", json_path);

    char buffer[MESH_UPDATE_PAK_JSON_MAX];
    FILE *file = fopen(json_path, "rb");
    if (file == NULL) {
        return;
    }
    const size_t length = fread(buffer, 1U, sizeof buffer - 1U, file);
    const bool oversized = !feof(file);
    fclose(file);
    if (length == 0U || oversized) {
        mesh_log_warn("update", "%s is not a pak.json we can rewrite", json_path);
        return;
    }
    buffer[length] = '\0';

    /* "version" : "v1.13.0" - find the value's quotes, and replace only what is between them. */
    char *key = strstr(buffer, "\"version\"");
    char *value = NULL;
    if (key != NULL) {
        char *colon = strchr(key + strlen("\"version\""), ':');
        value = colon != NULL ? strchr(colon, '"') : NULL;
    }
    char *end = value != NULL ? strchr(value + 1, '"') : NULL;
    if (end == NULL) {
        mesh_log_warn("update", "%s has no version field to stamp", json_path);
        return;
    }

    file = fopen(temp_path, "wb");
    if (file == NULL) {
        mesh_log_warn("update", "Could not write %s: %s", temp_path, strerror(errno));
        return;
    }
    const size_t head = (size_t)(value + 1 - buffer);
    const size_t tail = length - (size_t)(end - buffer);
    const bool wrote = fwrite(buffer, 1U, head, file) == head &&
                       fprintf(file, "v%s", updater->latest) > 0 &&
                       fwrite(end, 1U, tail, file) == tail;
    const bool closed = fclose(file) == 0;
    if (!wrote || !closed) {
        mesh_log_warn("update", "Could not write %s", temp_path);
        (void)unlink(temp_path);
        return;
    }
    if (rename(temp_path, json_path) != 0) {
        mesh_log_warn("update", "Could not replace %s: %s", json_path, strerror(errno));
        (void)unlink(temp_path);
        return;
    }
    mesh_log_info("update", "Stamped %s with v%s", json_path, updater->latest);
}

static void updater_on_download_done(void *userdata, const struct mesh_fetch_result *result) {
    struct mesh_updater *updater = (struct mesh_updater *)userdata;
    if (updater == NULL || updater->state != MESH_UPDATE_DOWNLOADING) {
        return;
    }
    if (result->outcome != MESH_FETCH_OK) {
        updater_fetch_failed(updater, result, MESH_STR_UPDATE_DOWNLOAD_EXIT);
        (void)unlink(updater->staged_path);
        return;
    }

    updater_set(updater, MESH_UPDATE_VERIFYING, mesh_str(MESH_STR_UPDATE_VERIFYING));

    struct stat info;
    if (stat(updater->staged_path, &info) != 0 || info.st_size <= 0) {
        updater_set(updater, MESH_UPDATE_FAILED, mesh_str(MESH_STR_UPDATE_FILE_MISSING));
        (void)unlink(updater->staged_path);
        return;
    }
    if (updater->asset_size != 0U && (uint64_t)info.st_size != updater->asset_size) {
        updater_set(updater, MESH_UPDATE_FAILED, mesh_str(MESH_STR_UPDATE_FILE_WRONG_SIZE));
        (void)unlink(updater->staged_path);
        return;
    }

    uint8_t digest[MESH_SHA256_DIGEST_LEN];
    const int hashed = mesh_sha256_file(updater->staged_path, digest);
    if (hashed != 0) {
        updater_set(updater, MESH_UPDATE_FAILED, mesh_str(MESH_STR_UPDATE_HASH_FAILED));
        (void)unlink(updater->staged_path);
        return;
    }
    char hex[MESH_SHA256_HEX_LEN];
    mesh_sha256_hex(digest, hex, sizeof hex);
    if (strcmp(hex, updater->asset_sha256) != 0) {
        mesh_log_warn("update", "Checksum mismatch: got %s, expected %s", hex,
                      updater->asset_sha256);
        updater_set(updater, MESH_UPDATE_FAILED, mesh_str(MESH_STR_UPDATE_CHECKSUM_MISMATCH));
        (void)unlink(updater->staged_path);
        return;
    }

    if (chmod(updater->staged_path, 0755) != 0) {
        updater_set(updater, MESH_UPDATE_FAILED, mesh_str(MESH_STR_UPDATE_CHMOD_FAILED));
        (void)unlink(updater->staged_path);
        return;
    }
    /* Atomic within the directory: readers see the old binary or the new one. The running
       image lives on through its inode, so this is safe under ourselves. */
    if (rename(updater->staged_path, updater->install_path) != 0) {
        char message[MESH_UPDATE_MESSAGE_MAX];
        mesh_str_format(message, sizeof message, MESH_STR_UPDATE_INSTALL_FAILED, strerror(errno));
        updater_set(updater, MESH_UPDATE_FAILED, message);
        (void)unlink(updater->staged_path);
        return;
    }

    mesh_log_info("update", "Installed %s over %s", updater->latest, updater->install_path);
    updater_stamp_pak_json(updater);
    char message[MESH_UPDATE_MESSAGE_MAX];
    mesh_str_format(message, sizeof message, MESH_STR_UPDATE_INSTALLED, updater->latest);
    updater_set(updater, MESH_UPDATE_READY, message);
}

/*
 * How much of the asset is on disk, if it has changed since the last look.
 *
 * The whole of the byte-progress mechanism, and it is four lines because the download's
 * destination is a file this process named - see `downloaded` in the header for why that is the
 * answer rather than reading curl's own meter.
 *
 * A missing file is not an error: between the fork and the fetcher's first write there is a
 * moment with nothing there, and the honest reading for that moment is zero. The revision is
 * bumped only on a change, so a stalled download does not republish a snapshot every turn.
 */
static void updater_sample_download(struct mesh_updater *updater) {
    struct stat info;
    uint64_t size = 0U;
    if (stat(updater->staged_path, &info) == 0 && info.st_size > 0) {
        size = (uint64_t)info.st_size;
    }
    /* A file longer than the release said it would be is a download that is no longer the asset
       we asked for; the digest will refuse it in a moment. Until then, report it as complete
       rather than as a fraction over one. */
    if (updater->asset_size > 0U && size > updater->asset_size) {
        size = updater->asset_size;
    }
    if (size != updater->downloaded) {
        updater->downloaded = size;
        updater->revision++;
    }
}

bool mesh_updater_progress(const struct mesh_updater *updater, uint32_t *permille) {
    if (permille != NULL) {
        *permille = 0U;
    }
    if (updater == NULL || updater->state != MESH_UPDATE_DOWNLOADING || updater->asset_size == 0U) {
        return false;
    }
    if (permille != NULL) {
        const uint64_t value = updater->downloaded * 1000U / updater->asset_size;
        *permille = value > 1000U ? 1000U : (uint32_t)value;
    }
    return true;
}

void mesh_updater_tick(struct mesh_updater *updater, uint64_t now_ms) {
    if (updater == NULL || !mesh_fetch_busy(&updater->fetch)) {
        return;
    }
    /* Before the child is reaped below, not after: the last sample of a download that has just
       finished is the one that puts the bar at the end, and a tick that reaped first would
       leave it stopped at whatever the second-to-last turn saw. */
    if (updater->state == MESH_UPDATE_DOWNLOADING) {
        updater_sample_download(updater);
    }
    /* Reaps a finished child and enforces the deadline; either can land in one of the two
       completions above, which is where a timed-out download unlinks its staging file. */
    mesh_fetch_tick(&updater->fetch, now_ms);
}
