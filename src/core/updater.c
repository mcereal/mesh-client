#define _POSIX_C_SOURCE 200809L

#include "mesh/updater.h"

#include "mesh/event_loop.h"
#include "mesh/log.h"
#include "mesh/sha256.h"
#include "mesh/version.h"

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
        return "idle";
    case MESH_UPDATE_CHECKING:
        return "checking";
    case MESH_UPDATE_UP_TO_DATE:
        return "up to date";
    case MESH_UPDATE_AVAILABLE:
        return "available";
    case MESH_UPDATE_DOWNLOADING:
        return "downloading";
    case MESH_UPDATE_VERIFYING:
        return "verifying";
    case MESH_UPDATE_READY:
        return "ready";
    case MESH_UPDATE_FAILED:
        return "failed";
    default:
        return "?";
    }
}

static void updater_set(struct mesh_updater *updater, enum mesh_update_state state,
                        const char *message) {
    updater->state = state;
    snprintf(updater->message, sizeof updater->message, "%s", message != NULL ? message : "");
    updater->revision++;
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

/* ---- child processes -------------------------------------------------------------------- */

static bool have_executable(const char *name) {
    const char *path = getenv("PATH");
    if (path == NULL || path[0] == '\0') {
        path = "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";
    }
    while (*path != '\0') {
        const char *colon = strchr(path, ':');
        const size_t len = colon != NULL ? (size_t)(colon - path) : strlen(path);
        if (len > 0U && len < 200U) {
            char candidate[256];
            snprintf(candidate, sizeof candidate, "%.*s/%s", (int)len, path, name);
            if (access(candidate, X_OK) == 0) {
                return true;
            }
        }
        if (colon == NULL) {
            break;
        }
        path = colon + 1;
    }
    return false;
}

/*
 * Drops the pipe but keeps the pid. Once the child has closed stdout there is nothing more to
 * read, and leaving the fd registered would be actively harmful: epoll reports EOF/HUP on
 * every wait, so mesh_event_loop_run() would never see a zero-event timeout, never return, and
 * never let mesh_updater_tick() enforce the deadline - a child that closed stdout without
 * exiting would spin the loop and freeze the UI. The response buffer is left alone; the step
 * that started the child still has to parse it.
 */
static void updater_release_fd(struct mesh_updater *updater) {
    if (updater->child_fd < 0) {
        return;
    }
    if (updater->loop != NULL) {
        (void)mesh_event_loop_remove_fd(updater->loop, updater->child_fd);
    }
    close(updater->child_fd);
    updater->child_fd = -1;
}

static void updater_close_child(struct mesh_updater *updater) {
    updater_release_fd(updater);
    if (updater->child > 0) {
        int status = 0;
        /* The caller has already decided the outcome; make sure the child is gone either way. */
        if (waitpid(updater->child, &status, WNOHANG) == 0) {
            kill(updater->child, SIGKILL);
            (void)waitpid(updater->child, &status, 0);
        }
        updater->child = -1;
    }
    free(updater->response);
    updater->response = NULL;
    updater->response_len = 0U;
}

static int updater_on_child_output(int fd, uint32_t events, void *userdata);

/*
 * Forks `argv` with its stdout on a pipe registered with the event loop. `capture` says
 * whether the output is wanted (the check) or only the exit status (the download).
 */
static int updater_spawn(struct mesh_updater *updater, char *const argv[], uint64_t now_ms,
                         uint32_t timeout_ms) {
    int fds[2];
    if (pipe(fds) < 0) {
        return -errno;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        const int err = -errno;
        close(fds[0]);
        close(fds[1]);
        return err;
    }
    if (pid == 0) {
        /* Child: stdout to the pipe, stderr to the log's fate (inherited), stdin closed. */
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(fds[1]);
        const int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    close(fds[1]);
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) {
        const int err = -errno;
        close(fds[0]);
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return err;
    }

    updater->child = pid;
    updater->child_fd = fds[0];
    updater->child_deadline_ms = now_ms + timeout_ms;
    if (updater->loop != NULL) {
        const int added = mesh_event_loop_add_fd(updater->loop, fds[0], EPOLLIN,
                                                 updater_on_child_output, updater);
        if (added != 0) {
            updater_close_child(updater);
            return added;
        }
    }
    return 0;
}

/* Appends whatever the child has written, capped so a runaway response cannot grow without
   bound. Returns false when the cap is hit. */
static bool updater_absorb(struct mesh_updater *updater, const char *bytes, size_t len) {
    if (updater->response_len + len + 1U > MESH_UPDATE_RESPONSE_MAX) {
        return false;
    }
    char *grown = realloc(updater->response, updater->response_len + len + 1U);
    if (grown == NULL) {
        return false;
    }
    memcpy(grown + updater->response_len, bytes, len);
    updater->response_len += len;
    grown[updater->response_len] = '\0';
    updater->response = grown;
    return true;
}

static void updater_finish_check(struct mesh_updater *updater, int exit_status);
static void updater_finish_download(struct mesh_updater *updater, int exit_status);

/* Reads whatever is buffered without blocking. Returns false when the pipe hit EOF, i.e. the
   child has closed its stdout and there will never be more. */
static bool updater_drain(struct mesh_updater *updater) {
    if (updater->child_fd < 0) {
        return false;
    }
    for (;;) {
        char buffer[4096];
        const ssize_t got = read(updater->child_fd, buffer, sizeof buffer);
        if (got > 0) {
            if (!updater_absorb(updater, buffer, (size_t)got)) {
                updater_set(updater, MESH_UPDATE_FAILED, "Response too large");
                updater_close_child(updater);
                return false;
            }
            continue;
        }
        if (got == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        updater_set(updater, MESH_UPDATE_FAILED, "Read from downloader failed");
        updater_close_child(updater);
        return false;
    }
}

/*
 * Finishes the step if the child has actually exited. Deliberately never blocks in waitpid:
 * this runs from the event loop, which is the same thread the UI draws on, and a child that
 * closed stdout without exiting would otherwise stall the whole client past the point where
 * the timeout in tick() could rescue it. If the child is not reaped yet, tick() picks it up on
 * a later turn or kills it at the deadline.
 *
 * Everything buffered is drained before dispatching, because the exit and the EOF are separate
 * events and either can be seen first: reaping a check without draining would parse a truncated
 * response as a broken release.
 */
static void updater_try_finish(struct mesh_updater *updater) {
    if (updater->child <= 0) {
        return;
    }
    if (!updater_drain(updater)) {
        /* EOF, or the drain tore the child down. Either way the pipe is finished with. */
        updater_release_fd(updater);
    }
    if (updater->child <= 0) {
        return; /* drain failed and tore the child down */
    }

    int status = 0;
    if (waitpid(updater->child, &status, WNOHANG) != updater->child) {
        return;
    }
    const int exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    updater->child = -1;

    const enum mesh_update_state state = updater->state;
    if (state == MESH_UPDATE_CHECKING) {
        updater_finish_check(updater, exit_status);
    } else if (state == MESH_UPDATE_DOWNLOADING) {
        updater_finish_download(updater, exit_status);
    }
    updater_close_child(updater);
}

static int updater_on_child_output(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct mesh_updater *updater = (struct mesh_updater *)userdata;
    if (updater == NULL) {
        return 0;
    }
    /* EOF or a hangup means the child has closed stdout; either way, drain and see whether it
       has exited. try_finish() is a no-op until it has, so nothing here can block. */
    if (updater_drain(updater) && (events & (EPOLLHUP | EPOLLERR)) == 0U) {
        return 0;
    }
    updater_try_finish(updater);
    return 0;
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
 * Pick the CA bundle the fetcher will verify github.com against.
 *
 * The Brick has no system CA store - no /etc/ssl at all - so curl rejects every HTTPS request
 * with exit 60 and the updater could never do anything on the one device it ships for.
 * `--insecure` is not the way out: the release metadata is what carries the digest the download
 * is checked against, so trusting it over an unauthenticated channel would defeat the
 * verification rather than work around a missing file. The pak therefore ships its own bundle
 * in certs/, and this finds it.
 *
 * Order: an explicit environment override first (curl honours SSL_CERT_FILE and CURL_CA_BUNDLE
 * itself, so this only records what is already in effect), then our own bundle, then the usual
 * system locations so a desktop build keeps using the distribution's certificates. Finding
 * nothing is not fatal - curl may have a built-in default - but it lets updater_fetch_failed()
 * say something more useful than "exit 60" when it turns out there was none.
 */
static void updater_resolve_ca_bundle(struct mesh_updater *updater) {
    updater->ca_bundle[0] = '\0';

    static const char *const k_env[] = {"SSL_CERT_FILE", "CURL_CA_BUNDLE"};
    for (size_t i = 0; i < sizeof k_env / sizeof k_env[0]; ++i) {
        const char *const value = getenv(k_env[i]);
        if (value != NULL && value[0] != '\0' && access(value, R_OK) == 0) {
            snprintf(updater->ca_bundle, sizeof updater->ca_bundle, "%s", value);
            return;
        }
    }

    if (updater_pak_file(updater->install_path, "certs/certificates.crt", updater->ca_bundle,
                         sizeof updater->ca_bundle)) {
        return;
    }

    static const char *const k_system[] = {
        "/etc/ssl/certs/ca-certificates.crt", /* Debian, Ubuntu, Alpine, Arch */
        "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora, RHEL */
        "/etc/ssl/cert.pem",                  /* BSD, and Alpine's compatibility link */
        "/etc/ssl/certs/ca-bundle.crt",
    };
    for (size_t i = 0; i < sizeof k_system / sizeof k_system[0]; ++i) {
        if (access(k_system[i], R_OK) == 0) {
            snprintf(updater->ca_bundle, sizeof updater->ca_bundle, "%s", k_system[i]);
            return;
        }
    }
}

/*
 * The one line the About screen shows when a fetcher exits non-zero.
 *
 * curl's 60 is specifically "peer certificate cannot be authenticated", which on a device with
 * no CA store is the only thing that will ever happen and which "exit 60" tells nobody how to
 * fix. The bundle ships in the pak and not through self-update, so the answer really is to
 * reinstall the pak.
 */
static void updater_fetch_failed(struct mesh_updater *updater, int exit_status, const char *what) {
    char message[MESH_UPDATE_MESSAGE_MAX];
    if (updater->fetcher != NULL && strcmp(updater->fetcher, "curl") == 0 && exit_status == 60) {
        snprintf(message, sizeof message, "%s",
                 updater->ca_bundle[0] != '\0' ? "Could not verify GitHub's certificate"
                                               : "No CA certificates; reinstall the pak");
    } else {
        snprintf(message, sizeof message, "%s failed (%s exit %d)", what,
                 updater->fetcher != NULL ? updater->fetcher : "fetcher", exit_status);
    }
    updater_set(updater, MESH_UPDATE_FAILED, message);
}

/* ---- steps ------------------------------------------------------------------------------ */

int mesh_updater_init(struct mesh_updater *updater, struct mesh_event_loop *loop) {
    if (updater == NULL) {
        return -EINVAL;
    }
    memset(updater, 0, sizeof *updater);
    updater->child = -1;
    updater->child_fd = -1;
    updater->loop = loop;
    updater->state = MESH_UPDATE_IDLE;

    if (have_executable("curl")) {
        updater->fetcher = "curl";
    } else if (have_executable("wget")) {
        updater->fetcher = "wget";
    } else {
        updater->fetcher = NULL;
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

    if (updater->fetcher == NULL) {
        snprintf(updater->message, sizeof updater->message, "%s", "No curl or wget on this device");
    } else if (!mesh_version_is_release()) {
        snprintf(updater->message, sizeof updater->message, "%s", "Development build");
    }
    mesh_log_info("update", "Updater ready: fetcher=%s binary=%s version=%s cacert=%s",
                  updater->fetcher != NULL ? updater->fetcher : "none",
                  updater->install_path[0] != '\0' ? updater->install_path : "unknown",
                  mesh_version_string(),
                  updater->ca_bundle[0] != '\0' ? updater->ca_bundle : "(fetcher default)");
    return 0;
}

void mesh_updater_shutdown(struct mesh_updater *updater) {
    if (updater == NULL) {
        return;
    }
    updater_close_child(updater);
    /* Half a download left behind would otherwise sit next to the binary until the next run. */
    if (updater->state == MESH_UPDATE_DOWNLOADING && updater->staged_path[0] != '\0') {
        (void)unlink(updater->staged_path);
    }
}

bool mesh_updater_available(const struct mesh_updater *updater) {
    return updater != NULL && updater->fetcher != NULL && updater->install_path[0] != '\0' &&
           updater->loop != NULL;
}

int mesh_updater_check(struct mesh_updater *updater, uint64_t now_ms) {
    if (updater == NULL) {
        return -EINVAL;
    }
    if (!mesh_updater_available(updater)) {
        return -ENOTSUP;
    }
    if (updater->child > 0) {
        return -EBUSY;
    }
    updater_close_child(updater);
    updater->latest[0] = '\0';
    updater->asset_url[0] = '\0';
    updater->asset_sha256[0] = '\0';
    updater->asset_size = 0U;

    /*
     * Which question to ask depends on the channel this build came from. `releases/latest`
     * deliberately skips prereleases, so a client built from beta or rc would only ever be
     * offered stable and would never see the next beta. `releases?per_page=1` is the newest
     * release of any kind - and capping it at one keeps the reply a single release object, so
     * the scanner below cannot pair one release's tag with another's asset.
     */
    char url[MESH_UPDATE_URL_MAX];
    if (mesh_version_is_release() && mesh_version_is_prerelease()) {
        snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases?per_page=1",
                 mesh_updater_repo());
    } else {
        snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest",
                 mesh_updater_repo());
    }
    char agent[64];
    snprintf(agent, sizeof agent, "meshclient/%s", mesh_version_string());

    int result;
    if (strcmp(updater->fetcher, "curl") == 0) {
        char agent_header[80];
        snprintf(agent_header, sizeof agent_header, "User-Agent: %s", agent);
        char *argv[16];
        size_t argc = 0U;
        argv[argc++] = (char *)"curl";
        argv[argc++] = (char *)"-fsSL";
        argv[argc++] = (char *)"--max-time";
        argv[argc++] = (char *)"25";
        if (updater->ca_bundle[0] != '\0') {
            argv[argc++] = (char *)"--cacert";
            argv[argc++] = updater->ca_bundle;
        }
        argv[argc++] = (char *)"-H";
        argv[argc++] = (char *)"Accept: application/vnd.github+json";
        argv[argc++] = (char *)"-H";
        argv[argc++] = agent_header;
        argv[argc++] = url;
        argv[argc] = NULL;
        result = updater_spawn(updater, argv, now_ms, MESH_UPDATE_CHECK_TIMEOUT_MS);
    } else {
        char agent_option[80];
        snprintf(agent_option, sizeof agent_option, "--user-agent=%s", agent);
        char ca_option[MESH_UPDATE_PATH_MAX + 24U];
        snprintf(ca_option, sizeof ca_option, "--ca-certificate=%s", updater->ca_bundle);
        char *argv[12];
        size_t argc = 0U;
        argv[argc++] = (char *)"wget";
        argv[argc++] = (char *)"-q";
        argv[argc++] = (char *)"-T";
        argv[argc++] = (char *)"25";
        if (updater->ca_bundle[0] != '\0') {
            argv[argc++] = ca_option;
        }
        argv[argc++] = agent_option;
        argv[argc++] = (char *)"-O";
        argv[argc++] = (char *)"-";
        argv[argc++] = url;
        argv[argc] = NULL;
        result = updater_spawn(updater, argv, now_ms, MESH_UPDATE_CHECK_TIMEOUT_MS);
    }
    if (result != 0) {
        updater_set(updater, MESH_UPDATE_FAILED, "Could not start the downloader");
        return result;
    }
    updater_set(updater, MESH_UPDATE_CHECKING, "Checking for updates");
    return 0;
}

static void updater_finish_check(struct mesh_updater *updater, int exit_status) {
    if (exit_status != 0) {
        updater_fetch_failed(updater, exit_status, "Check");
        return;
    }
    if (updater->response == NULL) {
        updater_set(updater, MESH_UPDATE_FAILED, "Empty reply from GitHub");
        return;
    }

    char tag[MESH_UPDATE_VERSION_MAX];
    char url[MESH_UPDATE_URL_MAX];
    char sha256[65];
    uint64_t size = 0U;
    if (!mesh_updater_parse_release(updater->response, mesh_updater_repo(),
                                    mesh_updater_asset_name(), tag, sizeof tag, url, sizeof url,
                                    sha256, sizeof sha256, &size)) {
        updater_set(updater, MESH_UPDATE_FAILED, "No usable release asset");
        return;
    }

    snprintf(updater->latest, sizeof updater->latest, "%s", tag);
    snprintf(updater->asset_url, sizeof updater->asset_url, "%s", url);
    snprintf(updater->asset_sha256, sizeof updater->asset_sha256, "%s", sha256);
    updater->asset_size = size;
    mesh_log_info("update", "Latest release %s (running %s), asset %llu bytes", tag,
                  mesh_version_string(), (unsigned long long)size);

    if (!mesh_version_is_release()) {
        char message[MESH_UPDATE_MESSAGE_MAX];
        snprintf(message, sizeof message, "Latest is %s; this is a dev build", tag);
        updater_set(updater, MESH_UPDATE_UP_TO_DATE, message);
        return;
    }
    if (!mesh_version_is_newer_than_running(tag)) {
        updater_set(updater, MESH_UPDATE_UP_TO_DATE, "Running the latest release");
        return;
    }
    if (updater->asset_sha256[0] == '\0') {
        /* Without a digest there is no way to tell a good download from a bad one, and this
           installs an executable. Refuse rather than trust the transport alone. */
        updater_set(updater, MESH_UPDATE_FAILED, "Release has no checksum; not installing");
        return;
    }
    if (size == 0U || size > MESH_UPDATE_MAX_ASSET_BYTES) {
        updater_set(updater, MESH_UPDATE_FAILED, "Release asset size looks wrong");
        return;
    }

    char message[MESH_UPDATE_MESSAGE_MAX];
    snprintf(message, sizeof message, "%s available (running %s)", tag, mesh_version_string());
    updater_set(updater, MESH_UPDATE_AVAILABLE, message);
}

int mesh_updater_install(struct mesh_updater *updater, uint64_t now_ms) {
    if (updater == NULL) {
        return -EINVAL;
    }
    if (!mesh_updater_available(updater)) {
        return -ENOTSUP;
    }
    if (updater->child > 0) {
        return -EBUSY;
    }
    if (updater->state != MESH_UPDATE_AVAILABLE || updater->asset_url[0] == '\0' ||
        updater->asset_sha256[0] == '\0') {
        return -EINVAL;
    }
    updater_close_child(updater);
    (void)unlink(updater->staged_path);

    int result;
    if (strcmp(updater->fetcher, "curl") == 0) {
        char *argv[12];
        size_t argc = 0U;
        argv[argc++] = (char *)"curl";
        argv[argc++] = (char *)"-fsSL";
        argv[argc++] = (char *)"--max-time";
        argv[argc++] = (char *)"280";
        if (updater->ca_bundle[0] != '\0') {
            argv[argc++] = (char *)"--cacert";
            argv[argc++] = updater->ca_bundle;
        }
        argv[argc++] = (char *)"-o";
        argv[argc++] = updater->staged_path;
        argv[argc++] = updater->asset_url;
        argv[argc] = NULL;
        result = updater_spawn(updater, argv, now_ms, MESH_UPDATE_DOWNLOAD_TIMEOUT_MS);
    } else {
        char ca_option[MESH_UPDATE_PATH_MAX + 24U];
        snprintf(ca_option, sizeof ca_option, "--ca-certificate=%s", updater->ca_bundle);
        char *argv[12];
        size_t argc = 0U;
        argv[argc++] = (char *)"wget";
        argv[argc++] = (char *)"-q";
        argv[argc++] = (char *)"-T";
        argv[argc++] = (char *)"280";
        if (updater->ca_bundle[0] != '\0') {
            argv[argc++] = ca_option;
        }
        argv[argc++] = (char *)"-O";
        argv[argc++] = updater->staged_path;
        argv[argc++] = updater->asset_url;
        argv[argc] = NULL;
        result = updater_spawn(updater, argv, now_ms, MESH_UPDATE_DOWNLOAD_TIMEOUT_MS);
    }
    if (result != 0) {
        updater_set(updater, MESH_UPDATE_FAILED, "Could not start the download");
        return result;
    }
    char message[MESH_UPDATE_MESSAGE_MAX];
    snprintf(message, sizeof message, "Downloading %s", updater->latest);
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

static void updater_finish_download(struct mesh_updater *updater, int exit_status) {
    if (exit_status != 0) {
        updater_fetch_failed(updater, exit_status, "Download");
        (void)unlink(updater->staged_path);
        return;
    }

    updater_set(updater, MESH_UPDATE_VERIFYING, "Verifying download");

    struct stat info;
    if (stat(updater->staged_path, &info) != 0 || info.st_size <= 0) {
        updater_set(updater, MESH_UPDATE_FAILED, "Downloaded file is missing");
        (void)unlink(updater->staged_path);
        return;
    }
    if (updater->asset_size != 0U && (uint64_t)info.st_size != updater->asset_size) {
        updater_set(updater, MESH_UPDATE_FAILED, "Downloaded file is the wrong size");
        (void)unlink(updater->staged_path);
        return;
    }

    uint8_t digest[MESH_SHA256_DIGEST_LEN];
    const int hashed = mesh_sha256_file(updater->staged_path, digest);
    if (hashed != 0) {
        updater_set(updater, MESH_UPDATE_FAILED, "Could not hash the download");
        (void)unlink(updater->staged_path);
        return;
    }
    char hex[MESH_SHA256_HEX_LEN];
    mesh_sha256_hex(digest, hex, sizeof hex);
    if (strcmp(hex, updater->asset_sha256) != 0) {
        mesh_log_warn("update", "Checksum mismatch: got %s, expected %s", hex,
                      updater->asset_sha256);
        updater_set(updater, MESH_UPDATE_FAILED, "Checksum mismatch; discarded");
        (void)unlink(updater->staged_path);
        return;
    }

    if (chmod(updater->staged_path, 0755) != 0) {
        updater_set(updater, MESH_UPDATE_FAILED, "Could not make the update executable");
        (void)unlink(updater->staged_path);
        return;
    }
    /* Atomic within the directory: readers see the old binary or the new one. The running
       image lives on through its inode, so this is safe under ourselves. */
    if (rename(updater->staged_path, updater->install_path) != 0) {
        char message[MESH_UPDATE_MESSAGE_MAX];
        snprintf(message, sizeof message, "Install failed: %s", strerror(errno));
        updater_set(updater, MESH_UPDATE_FAILED, message);
        (void)unlink(updater->staged_path);
        return;
    }

    mesh_log_info("update", "Installed %s over %s", updater->latest, updater->install_path);
    updater_stamp_pak_json(updater);
    char message[MESH_UPDATE_MESSAGE_MAX];
    snprintf(message, sizeof message, "%s installed - relaunch to run it", updater->latest);
    updater_set(updater, MESH_UPDATE_READY, message);
}

void mesh_updater_tick(struct mesh_updater *updater, uint64_t now_ms) {
    if (updater == NULL || updater->child <= 0) {
        return;
    }
    /* A child that exited without closing stdout, or whose EOF the loop did not deliver, is
       finished here. Drains first, exactly as the fd callback does. */
    updater_try_finish(updater);
    if (updater->child <= 0) {
        return;
    }

    if (now_ms >= updater->child_deadline_ms) {
        mesh_log_warn("update", "%s timed out in state %s", updater->fetcher,
                      mesh_update_state_name(updater->state));
        const bool downloading = updater->state == MESH_UPDATE_DOWNLOADING;
        updater_close_child(updater);
        if (downloading) {
            (void)unlink(updater->staged_path);
        }
        updater_set(updater, MESH_UPDATE_FAILED, "Timed out talking to GitHub");
    }
}
