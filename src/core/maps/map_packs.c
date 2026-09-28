#include "mesh/core/map_packs.h"

#include "inkwell/base/file.h"
#include "inkwell/base/log.h"
#include "inkwell/codec/json.h"

#include "mesh/i18n/net_reason.h"
#include "mesh/i18n/strings.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define PACKS_SUFFIX ".mctp"
#define PACKS_PART_SUFFIX ".mctp.part"
#define PACKS_PIECE_SUFFIX ".mctp.piece"
#define PACKS_HASH_SUFFIX ".mctp.hash"

/* How long one piece may take, and how long it may sit silent. A piece is 4 MB, so the first is
   a slow link's worth; the second is what tells a server that took the request and went quiet
   from one that is merely slow. */
#define PACKS_PIECE_TIMEOUT_MS 180000U
#define PACKS_PIECE_IDLE_MS 30000U
#define PACKS_CATALOG_TIMEOUT_MS 30000U

/* The digest-so-far file: what a resume needs to carry on hashing rather than read back the
   whole `.part`. A raw struct, because it is only ever read back by this binary - and a file
   from another build, whose struct may differ, is caught by the size and thrown away, which
   costs a download from the start rather than a wrong digest. */
#define PACKS_HASH_MAGIC "MCTPHASH"
struct packs_hash_file {
    char magic[8];
    uint32_t version;
    uint32_t digest_size;
    uint64_t have;
    struct inkwell_sha256 digest;
};

const char *mesh_map_packs_state_name(enum mesh_map_packs_state state) {
    switch (state) {
    case MESH_MAP_PACKS_IDLE:
        return "idle";
    case MESH_MAP_PACKS_LOADING:
        return "loading";
    case MESH_MAP_PACKS_READY:
        return "ready";
    case MESH_MAP_PACKS_DOWNLOADING:
        return "downloading";
    case MESH_MAP_PACKS_FAILED:
        return "failed";
    case MESH_MAP_PACKS_STATE_COUNT:
    default:
        return "unknown";
    }
}

/* ---- the catalog ---------------------------------------------------------------------------- */

/* Lowercase letters, digits and single hyphens: an id is a file name on a FAT32 card and a path
   on the server, so anything else is refused rather than escaped. */
static bool packs_id_ok(const char *id) {
    if (id[0] == '\0' || id[0] == '-') {
        return false;
    }
    for (const char *at = id; *at != '\0'; ++at) {
        const bool ok = (*at >= 'a' && *at <= 'z') || (*at >= '0' && *at <= '9') ||
                        (*at == '-' && at[1] != '-' && at[1] != '\0');
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool packs_cut_ok(const char *cut) {
    if (strlen(cut) != 8U) {
        return false;
    }
    for (size_t i = 0U; i < 8U; ++i) {
        if (!isdigit((unsigned char)cut[i])) {
            return false;
        }
    }
    return true;
}

static bool packs_sha_ok(const char *sha) {
    if (strlen(sha) != 64U) {
        return false;
    }
    for (size_t i = 0U; i < 64U; ++i) {
        if (!isdigit((unsigned char)sha[i]) && !(sha[i] >= 'a' && sha[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

/* A path under the catalog's own directory: no scheme, no leading slash, no way up. */
static bool packs_url_ok(const char *url) {
    if (url[0] == '\0' || url[0] == '/' || strstr(url, "..") != NULL) {
        return false;
    }
    for (const char *at = url; *at != '\0'; ++at) {
        const bool ok = (*at >= 'a' && *at <= 'z') || (*at >= '0' && *at <= '9') || *at == '-' ||
                        *at == '_' || *at == '.' || *at == '/';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool packs_read_group(struct inkwell_json *json, struct mesh_map_packs_group *group) {
    memset(group, 0, sizeof *group);
    if (!inkwell_json_enter_object(json)) {
        return false;
    }
    char key[16];
    while (inkwell_json_next_key(json, key, sizeof key)) {
        bool read = false;
        if (strcmp(key, "id") == 0) {
            read = inkwell_json_read_string(json, group->id, sizeof group->id);
        } else if (strcmp(key, "name") == 0) {
            read = inkwell_json_read_string(json, group->name, sizeof group->name);
        } else if (strcmp(key, "parent") == 0) {
            /* null for the top, which the string reader refuses and the skip below steps over */
            read = inkwell_json_read_string(json, group->parent, sizeof group->parent);
        }
        if (!read && !inkwell_json_skip_value(json)) {
            return false;
        }
    }
    return true;
}

static bool packs_read_entry(struct inkwell_json *json, struct mesh_map_packs_entry *entry) {
    memset(entry, 0, sizeof *entry);
    if (!inkwell_json_enter_object(json)) {
        return false;
    }
    char key[16];
    while (inkwell_json_next_key(json, key, sizeof key)) {
        bool read = false;
        uint64_t number = 0U;
        if (strcmp(key, "id") == 0) {
            read = inkwell_json_read_string(json, entry->id, sizeof entry->id);
        } else if (strcmp(key, "name") == 0) {
            read = inkwell_json_read_string(json, entry->name, sizeof entry->name);
        } else if (strcmp(key, "parent") == 0) {
            read = inkwell_json_read_string(json, entry->parent, sizeof entry->parent);
        } else if (strcmp(key, "cut") == 0) {
            read = inkwell_json_read_string(json, entry->cut, sizeof entry->cut);
        } else if (strcmp(key, "sha256") == 0) {
            read = inkwell_json_read_string(json, entry->sha256, sizeof entry->sha256);
        } else if (strcmp(key, "url") == 0) {
            read = inkwell_json_read_string(json, entry->url, sizeof entry->url);
        } else if (strcmp(key, "bytes") == 0) {
            read = inkwell_json_read_u64(json, &entry->bytes);
        } else if (strcmp(key, "tiles") == 0) {
            read = inkwell_json_read_u64(json, &number);
            entry->tiles = number > UINT32_MAX ? UINT32_MAX : (uint32_t)number;
        } else if (strcmp(key, "max_zoom") == 0) {
            read = inkwell_json_read_u64(json, &number);
            entry->max_zoom = number > UINT8_MAX ? UINT8_MAX : (uint8_t)number;
        }
        if (!read && !inkwell_json_skip_value(json)) {
            return false;
        }
    }
    return true;
}

/* Only the light style is drawn today; a catalog may list others for the same id later, and
   this client takes the first of each id it can use rather than two entries of one region. */
bool mesh_map_packs_catalog_parse(const char *text, size_t len,
                                  struct mesh_map_packs_catalog *out) {
    if (text == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    struct inkwell_json json;
    inkwell_json_init(&json, text, len);
    if (!inkwell_json_enter_object(&json)) {
        return false;
    }
    bool format_ok = false;
    char key[16];
    while (inkwell_json_next_key(&json, key, sizeof key)) {
        if (strcmp(key, "format") == 0) {
            uint64_t format = 0U;
            if (!inkwell_json_read_u64(&json, &format)) {
                return false;
            }
            format_ok = format == MESH_MAP_PACKS_FORMAT;
        } else if (strcmp(key, "groups") == 0) {
            if (!inkwell_json_enter_array(&json)) {
                return false;
            }
            while (inkwell_json_next_element(&json)) {
                struct mesh_map_packs_group group;
                if (!packs_read_group(&json, &group)) {
                    return false;
                }
                if (packs_id_ok(group.id) && group.name[0] != '\0' &&
                    out->group_count < MESH_MAP_PACKS_GROUPS_MAX) {
                    out->groups[out->group_count++] = group;
                }
            }
        } else if (strcmp(key, "packs") == 0) {
            if (!inkwell_json_enter_array(&json)) {
                return false;
            }
            while (inkwell_json_next_element(&json)) {
                struct mesh_map_packs_entry entry;
                if (!packs_read_entry(&json, &entry)) {
                    return false;
                }
                const bool usable = packs_id_ok(entry.id) && entry.name[0] != '\0' &&
                                    packs_cut_ok(entry.cut) && packs_sha_ok(entry.sha256) &&
                                    packs_url_ok(entry.url) && entry.bytes > 0U;
                bool seen = false;
                for (size_t i = 0U; i < out->entry_count; ++i) {
                    seen = seen || strcmp(out->entries[i].id, entry.id) == 0;
                }
                if (usable && !seen && out->entry_count < MESH_MAP_PACKS_ENTRIES_MAX) {
                    out->entries[out->entry_count++] = entry;
                }
            }
        } else if (!inkwell_json_skip_value(&json)) {
            return false;
        }
    }
    /* Read after the walk, since nothing says `format` comes first. */
    return format_ok;
}

/* ---- the card ------------------------------------------------------------------------------- */

static void packs_changed(struct mesh_map_packs *packs) { ++packs->revision; }

static void packs_set(struct mesh_map_packs *packs, enum mesh_map_packs_state state,
                      const char *message) {
    packs->state = state;
    snprintf(packs->message, sizeof packs->message, "%s", message != NULL ? message : "");
    packs_changed(packs);
}

static bool packs_ends_with(const char *name, const char *suffix) {
    const size_t length = strlen(name);
    const size_t tail = strlen(suffix);
    return length > tail && strcasecmp(name + length - tail, suffix) == 0;
}

/* Splits `<id>.<cut>.mctp` into its halves; false for any other name. */
static bool packs_split_name(const char *name, char *id, size_t id_len, char *cut, size_t cut_len) {
    const size_t length = strlen(name);
    const size_t tail = sizeof PACKS_SUFFIX - 1U;
    if (length < tail + 10U || !packs_ends_with(name, PACKS_SUFFIX)) {
        return false;
    }
    const size_t stem = length - tail;
    if (name[stem - 9U] != '.' || stem - 9U >= id_len || cut_len < 9U) {
        return false;
    }
    memcpy(cut, name + stem - 8U, 8U);
    cut[8] = '\0';
    memcpy(id, name, stem - 9U);
    id[stem - 9U] = '\0';
    return packs_cut_ok(cut) && packs_id_ok(id);
}

static uint64_t packs_file_size(const char *path, bool *exists) {
    struct stat info;
    const bool there = stat(path, &info) == 0;
    if (exists != NULL) {
        *exists = there;
    }
    return there && info.st_size > 0 ? (uint64_t)info.st_size : 0U;
}

struct packs_scan {
    struct mesh_map_packs *packs;
};

static void packs_scan_visit(void *context, const char *name) {
    struct mesh_map_packs *const packs = ((struct packs_scan *)context)->packs;
    /* The same rule the map opens by (mesh/map/stack.h): `.mctp` in any case, no dot files. */
    if (name[0] == '.' || !packs_ends_with(name, PACKS_SUFFIX) ||
        strlen(name) >= MESH_MAP_PACKS_NAME_ON_CARD_MAX) {
        return;
    }
    /* Counted whether or not it can be listed: a pack the map draws is one of the
       MESH_MAP_PACKS_DRAWN_MAX however long its name. */
    ++packs->on_card;
    struct mesh_map_packs_installed *const pack = &packs->installed[packs->installed_count];
    const size_t stem = strlen(name) - (sizeof PACKS_SUFFIX - 1U);
    if (packs->installed_count >= MESH_MAP_PACKS_INSTALLED_MAX ||
        strlen(name) >= sizeof pack->file || stem >= sizeof pack->id) {
        return;
    }
    memset(pack, 0, sizeof *pack);
    snprintf(pack->file, sizeof pack->file, "%s", name);
    if (!packs_split_name(name, pack->id, sizeof pack->id, pack->cut, sizeof pack->cut)) {
        snprintf(pack->id, sizeof pack->id, "%.*s", (int)stem, name);
        pack->cut[0] = '\0';
    }
    char path[MESH_MAP_PACKS_PATH_MAX + 64U];
    snprintf(path, sizeof path, "%s/%s", packs->dir, name);
    pack->bytes = packs_file_size(path, NULL);
    ++packs->installed_count;
}

static int packs_compare_installed(const void *a, const void *b) {
    return strcmp(((const struct mesh_map_packs_installed *)a)->id,
                  ((const struct mesh_map_packs_installed *)b)->id);
}

void mesh_map_packs_rescan(struct mesh_map_packs *packs) {
    if (packs == NULL) {
        return;
    }
    packs->installed_count = 0U;
    packs->on_card = 0U;
    struct packs_scan scan = {.packs = packs};
    if (packs->dir[0] != '\0' && inkwell_file_is_dir(packs->dir)) {
        (void)inkwell_file_list(packs->dir, packs_scan_visit, &scan);
    }
    qsort(packs->installed, packs->installed_count, sizeof packs->installed[0],
          packs_compare_installed);
    packs_changed(packs);
}

/* Makes the directory and the one above it. EEXIST is the ordinary case at both. */
static int packs_make_dir(const char *dir) {
    char parent[MESH_MAP_PACKS_PATH_MAX];
    snprintf(parent, sizeof parent, "%s", dir);
    char *const slash = strrchr(parent, '/');
    if (slash != NULL && slash != parent) {
        *slash = '\0';
        if (!inkwell_file_is_dir(parent) && inkwell_file_mkdir(parent) != 0 && errno != EEXIST) {
            return -errno;
        }
    }
    if (!inkwell_file_is_dir(dir) && inkwell_file_mkdir(dir) != 0 && errno != EEXIST) {
        return -errno;
    }
    return 0;
}

/* ---- lifecycle ------------------------------------------------------------------------------ */

int mesh_map_packs_init(struct mesh_map_packs *packs, struct inkwell_loop *loop, const char *dir) {
    if (packs == NULL) {
        return -EINVAL;
    }
    memset(packs, 0, sizeof *packs);
    snprintf(packs->dir, sizeof packs->dir, "%s", dir != NULL ? dir : "");
    snprintf(packs->catalog_url, sizeof packs->catalog_url, "%s", MESH_MAP_PACKS_CATALOG_URL);
    const int ready = inkwell_fetch_init(&packs->fetch, loop);
    if (!inkwell_fetch_available(&packs->fetch)) {
        snprintf(packs->message, sizeof packs->message, "%s",
                 inkcell_str(MESH_STR_MAP_PACKS_NO_TLS));
    }
    mesh_map_packs_rescan(packs);
    return ready;
}

void mesh_map_packs_shutdown(struct mesh_map_packs *packs) {
    if (packs == NULL) {
        return;
    }
    if (packs->state == MESH_MAP_PACKS_DOWNLOADING) {
        mesh_map_packs_cancel(packs);
    }
    inkwell_fetch_shutdown(&packs->fetch);
}

bool mesh_map_packs_available(const struct mesh_map_packs *packs) {
    return packs != NULL && packs->dir[0] != '\0' && inkwell_fetch_available(&packs->fetch);
}

void mesh_map_packs_tick(struct mesh_map_packs *packs, uint64_t now_ms) {
    if (packs != NULL) {
        packs->now_ms = now_ms;
        inkwell_fetch_tick(&packs->fetch, now_ms);
    }
}

void mesh_map_packs_connect_to(struct mesh_map_packs *packs, const char *host, uint16_t port) {
    if (packs != NULL) {
        inkwell_fetch_connect_to(&packs->fetch, host, port);
    }
}

void mesh_map_packs_set_catalog_url(struct mesh_map_packs *packs, const char *url) {
    if (packs != NULL && url != NULL) {
        snprintf(packs->catalog_url, sizeof packs->catalog_url, "%s", url);
    }
}

const struct mesh_map_packs_entry *mesh_map_packs_find(const struct mesh_map_packs *packs,
                                                       const char *id) {
    if (packs == NULL || id == NULL || !packs->catalog_valid) {
        return NULL;
    }
    for (size_t i = 0U; i < packs->catalog.entry_count; ++i) {
        if (strcmp(packs->catalog.entries[i].id, id) == 0) {
            return &packs->catalog.entries[i];
        }
    }
    return NULL;
}

const struct mesh_map_packs_installed *
mesh_map_packs_find_installed(const struct mesh_map_packs *packs, const char *id) {
    if (packs == NULL || id == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < packs->installed_count; ++i) {
        if (strcmp(packs->installed[i].id, id) == 0) {
            return &packs->installed[i];
        }
    }
    return NULL;
}

bool mesh_map_packs_progress(const struct mesh_map_packs *packs, uint16_t *permille) {
    if (packs == NULL || packs->state != MESH_MAP_PACKS_DOWNLOADING ||
        packs->download.entry.bytes == 0U) {
        return false;
    }
    if (permille != NULL) {
        /* What is verified, plus what of the piece in flight has reached the card - otherwise
           the bar moves once every four megabytes, which on a slow link is a bar that looks
           stuck. */
        uint64_t piece = packs_file_size(packs->download.chunk_path, NULL);
        if (piece > packs->download.chunk_len) {
            piece = packs->download.chunk_len;
        }
        uint64_t have = packs->download.have + piece;
        if (have > packs->download.entry.bytes) {
            have = packs->download.entry.bytes;
        }
        *permille = (uint16_t)((have * 1000U) / packs->download.entry.bytes);
    }
    return true;
}

bool mesh_map_packs_holds_the_antenna(const struct mesh_map_packs *packs) {
    return packs != NULL && packs->state == MESH_MAP_PACKS_DOWNLOADING;
}

/* ---- fetching ------------------------------------------------------------------------------- */

/* Why a request failed, in a sentence for the screen, and what happened for the log. */
static void packs_fetch_failed(struct mesh_map_packs *packs,
                               const struct inkwell_fetch_result *result) {
    char message[MESH_MAP_PACKS_MESSAGE_MAX];
    switch (result->outcome) {
    case INKWELL_FETCH_TOO_LARGE:
        snprintf(message, sizeof message, "%s", inkcell_str(MESH_STR_MAP_PACKS_TOO_LARGE));
        break;
    case INKWELL_FETCH_NETWORK:
        if (!mesh_net_reason_format(&result->failure, result->host, NULL, message,
                                    sizeof message)) {
            snprintf(message, sizeof message, "%s", inkcell_str(MESH_STR_MAP_PACKS_UNREACHABLE));
        }
        break;
    case INKWELL_FETCH_TLS:
        snprintf(message, sizeof message, "%s", inkcell_str(MESH_STR_MAP_PACKS_TLS_UNVERIFIED));
        break;
    case INKWELL_FETCH_FILE:
        snprintf(message, sizeof message, "%s", inkcell_str(MESH_STR_MAP_PACKS_WRITE_FAILED));
        break;
    case INKWELL_FETCH_TIMED_OUT:
        snprintf(message, sizeof message, "%s", inkcell_str(MESH_STR_MAP_PACKS_TIMED_OUT));
        break;
    case INKWELL_FETCH_HTTP_STATUS:
        inkcell_str_format(message, sizeof message, MESH_STR_MAP_PACKS_HTTP_STATUS, result->status);
        break;
    case INKWELL_FETCH_PROTOCOL:
    case INKWELL_FETCH_OK:
    case INKWELL_FETCH_OUTCOME_COUNT:
    default:
        snprintf(message, sizeof message, "%s", inkcell_str(MESH_STR_MAP_PACKS_BAD_REPLY));
        break;
    }
    inkwell_log_warn("maps", "Fetch failed while %s: %s/%s (%s)",
                     mesh_map_packs_state_name(packs->state),
                     inkwell_fetch_outcome_name(result->outcome),
                     inkwell_net_reason_name(result->failure.reason), result->detail);
    packs_set(packs, MESH_MAP_PACKS_FAILED, message);
}

static void packs_catalog_done(void *userdata, const struct inkwell_fetch_result *result) {
    struct mesh_map_packs *const packs = userdata;
    if (result->outcome != INKWELL_FETCH_OK) {
        packs_fetch_failed(packs, result);
        return;
    }
    struct mesh_map_packs_catalog *const parsed = calloc(1U, sizeof *parsed);
    if (parsed == NULL || !mesh_map_packs_catalog_parse(result->body, result->len, parsed)) {
        free(parsed);
        inkwell_log_warn("maps", "Catalog of %zu bytes could not be read", result->len);
        packs_set(packs, MESH_MAP_PACKS_FAILED, inkcell_str(MESH_STR_MAP_PACKS_BAD_CATALOG));
        return;
    }
    packs->catalog = *parsed;
    free(parsed);
    packs->catalog_valid = true;
    inkwell_log_info("maps", "Catalog: %zu packs in %zu groups", packs->catalog.entry_count,
                     packs->catalog.group_count);
    packs_set(packs, MESH_MAP_PACKS_READY, "");
}

int mesh_map_packs_refresh(struct mesh_map_packs *packs, uint64_t now_ms) {
    if (packs == NULL) {
        return -EINVAL;
    }
    if (!mesh_map_packs_available(packs)) {
        return -ENOTSUP;
    }
    if (inkwell_fetch_busy(&packs->fetch) || packs->state == MESH_MAP_PACKS_DOWNLOADING) {
        return -EBUSY;
    }
    const struct inkwell_fetch_request request = {
        .url = packs->catalog_url,
        .headers = {"Accept: application/json"},
        .timeout_ms = PACKS_CATALOG_TIMEOUT_MS,
        .response_max = MESH_MAP_PACKS_CATALOG_MAX,
        .on_done = packs_catalog_done,
        .userdata = packs,
    };
    const int started = inkwell_fetch_start(&packs->fetch, &request, now_ms);
    if (started < 0) {
        return started;
    }
    packs_set(packs, MESH_MAP_PACKS_LOADING, "");
    return 0;
}

/* ---- downloading ---------------------------------------------------------------------------- */

static void packs_download_path(const struct mesh_map_packs *packs, const char *suffix, char *out,
                                size_t out_len) {
    snprintf(out, out_len, "%s/%s.%s%s", packs->dir, packs->download.entry.id,
             packs->download.entry.cut, suffix);
}

static void packs_save_hash(const struct mesh_map_packs_download *download) {
    struct packs_hash_file file;
    memset(&file, 0, sizeof file);
    memcpy(file.magic, PACKS_HASH_MAGIC, sizeof file.magic);
    file.version = 1U;
    file.digest_size = (uint32_t)sizeof file.digest;
    file.have = download->have;
    file.digest = download->digest;
    char temp[MESH_MAP_PACKS_PATH_MAX + 8U];
    snprintf(temp, sizeof temp, "%s.tmp", download->state_path);
    FILE *const out = fopen(temp, "wb");
    if (out == NULL) {
        return;
    }
    const bool written = fwrite(&file, sizeof file, 1U, out) == 1U;
    if (fclose(out) == 0 && written) {
        (void)inkwell_file_replace(temp, download->state_path);
    } else {
        (void)remove(temp);
    }
}

/* The digest of the `.part` so far, when the file beside it says so and agrees with the
   `.part`'s length; otherwise a download that starts again. */
static bool packs_load_hash(struct mesh_map_packs_download *download, uint64_t part_bytes) {
    struct packs_hash_file file;
    FILE *const in = fopen(download->state_path, "rb");
    if (in == NULL) {
        return false;
    }
    const bool read = fread(&file, sizeof file, 1U, in) == 1U;
    (void)fclose(in);
    if (!read || memcmp(file.magic, PACKS_HASH_MAGIC, sizeof file.magic) != 0 ||
        file.version != 1U || file.digest_size != sizeof file.digest || file.have != part_bytes ||
        file.have > download->entry.bytes) {
        return false;
    }
    download->have = file.have;
    download->digest = file.digest;
    return true;
}

static void packs_next_piece(struct mesh_map_packs *packs, uint64_t now_ms);

/* Removes every file of `id` in the directory except `keep` (a file name, or NULL). */
static int packs_remove_id(struct mesh_map_packs *packs, const char *id, const char *keep);

static void packs_download_failed(struct mesh_map_packs *packs, const char *message) {
    (void)remove(packs->download.chunk_path);
    packs_set(packs, MESH_MAP_PACKS_FAILED, message);
}

static void packs_finish(struct mesh_map_packs *packs) {
    struct mesh_map_packs_download *const download = &packs->download;
    uint8_t digest[32];
    char hex[65];
    inkwell_sha256_final(&download->digest, digest);
    inkwell_sha256_hex(digest, hex, sizeof hex);
    if (strcmp(hex, download->entry.sha256) != 0) {
        /* The whole file is suspect, not the last piece: which piece went wrong is not
           something a digest of the whole can say. */
        inkwell_log_warn("maps", "%s: digest %s, catalog says %s", download->entry.id, hex,
                         download->entry.sha256);
        (void)remove(download->part_path);
        (void)remove(download->state_path);
        char message[MESH_MAP_PACKS_MESSAGE_MAX];
        inkcell_str_format(message, sizeof message, MESH_STR_MAP_PACKS_DAMAGED,
                           download->entry.name);
        packs_download_failed(packs, message);
        return;
    }
    char final_path[MESH_MAP_PACKS_PATH_MAX];
    packs_download_path(packs, PACKS_SUFFIX, final_path, sizeof final_path);
    if (inkwell_file_replace(download->part_path, final_path) != 0) {
        packs_download_failed(packs, inkcell_str(MESH_STR_MAP_PACKS_WRITE_FAILED));
        return;
    }
    (void)remove(download->state_path);
    /* The older cut goes after the new one is in place, so there is never a moment with
       neither. */
    char keep[MESH_MAP_PACKS_ID_MAX + MESH_MAP_PACKS_CUT_MAX + 8U];
    snprintf(keep, sizeof keep, "%s.%s%s", download->entry.id, download->entry.cut, PACKS_SUFFIX);
    packs_remove_id(packs, download->entry.id, keep);
    inkwell_log_info("maps", "%s %s installed, %" PRIu64 " bytes", download->entry.id,
                     download->entry.cut, download->entry.bytes);
    mesh_map_packs_rescan(packs);
    ++packs->installed_revision;
    char message[MESH_MAP_PACKS_MESSAGE_MAX];
    inkcell_str_format(message, sizeof message, MESH_STR_MAP_PACKS_DONE, download->entry.name);
    packs_set(packs, MESH_MAP_PACKS_READY, message);
}

/* Appends the piece that just arrived to the `.part`, hashing it on the way. */
static bool packs_append_piece(struct mesh_map_packs_download *download) {
    FILE *const in = fopen(download->chunk_path, "rb");
    FILE *const out = fopen(download->part_path, "ab");
    bool ok = in != NULL && out != NULL;
    uint64_t moved = 0U;
    static uint8_t buffer[64U * 1024U];
    while (ok) {
        const size_t got = fread(buffer, 1U, sizeof buffer, in);
        if (got == 0U) {
            ok = ferror(in) == 0;
            break;
        }
        if (fwrite(buffer, 1U, got, out) != got) {
            ok = false;
            break;
        }
        inkwell_sha256_update(&download->digest, buffer, got);
        moved += got;
    }
    if (in != NULL) {
        (void)fclose(in);
    }
    if (out != NULL && fclose(out) != 0) {
        ok = false;
    }
    (void)remove(download->chunk_path);
    if (!ok || moved != download->chunk_len) {
        return false;
    }
    download->have += moved;
    return true;
}

static void packs_piece_done(void *userdata, const struct inkwell_fetch_result *result) {
    struct mesh_map_packs *const packs = userdata;
    if (packs->state != MESH_MAP_PACKS_DOWNLOADING) {
        return;
    }
    if (result->outcome != INKWELL_FETCH_OK) {
        (void)remove(packs->download.chunk_path);
        packs_fetch_failed(packs, result);
        return;
    }
    if (!packs_append_piece(&packs->download)) {
        /* The `.part` may now hold part of this piece past what the digest covers, so neither
           can be trusted to resume from. */
        (void)remove(packs->download.part_path);
        (void)remove(packs->download.state_path);
        packs_download_failed(packs, inkcell_str(MESH_STR_MAP_PACKS_WRITE_FAILED));
        return;
    }
    packs_save_hash(&packs->download);
    packs_changed(packs);
    if (packs->download.have >= packs->download.entry.bytes) {
        packs_finish(packs);
        return;
    }
    packs_next_piece(packs, packs->now_ms);
}

static void packs_next_piece(struct mesh_map_packs *packs, uint64_t now_ms) {
    struct mesh_map_packs_download *const download = &packs->download;
    const uint64_t left = download->entry.bytes - download->have;
    download->chunk_len = left < MESH_MAP_PACKS_CHUNK_BYTES ? left : MESH_MAP_PACKS_CHUNK_BYTES;
    snprintf(download->range, sizeof download->range, "Range: bytes=%" PRIu64 "-%" PRIu64,
             download->have, download->have + download->chunk_len - 1U);
    const struct inkwell_fetch_request request = {
        .url = download->url,
        .headers = {download->range},
        .output_path = download->chunk_path,
        .output_max = download->chunk_len,
        .timeout_ms = PACKS_PIECE_TIMEOUT_MS,
        .idle_timeout_ms = PACKS_PIECE_IDLE_MS,
        .on_done = packs_piece_done,
        .userdata = packs,
    };
    const int started = inkwell_fetch_start(&packs->fetch, &request, now_ms);
    if (started < 0) {
        inkwell_log_warn("maps", "Piece at %" PRIu64 " of %s would not start: %s", download->have,
                         download->entry.id, strerror(-started));
        packs_download_failed(packs, inkcell_str(MESH_STR_MAP_PACKS_UNREACHABLE));
    }
}

int mesh_map_packs_download(struct mesh_map_packs *packs, const char *id, uint64_t now_ms) {
    if (packs == NULL || id == NULL) {
        return -EINVAL;
    }
    if (!mesh_map_packs_available(packs)) {
        return -ENOTSUP;
    }
    if (inkwell_fetch_busy(&packs->fetch) || packs->state == MESH_MAP_PACKS_DOWNLOADING) {
        return -EBUSY;
    }
    const struct mesh_map_packs_entry *const entry = mesh_map_packs_find(packs, id);
    if (entry == NULL) {
        return -ENOENT;
    }
    if (mesh_map_packs_find_installed(packs, id) == NULL &&
        packs->on_card >= MESH_MAP_PACKS_DRAWN_MAX) {
        return -ENOSPC;
    }
    const int made = packs_make_dir(packs->dir);
    if (made < 0) {
        return made;
    }

    struct mesh_map_packs_download *const download = &packs->download;
    memset(download, 0, sizeof *download);
    download->entry = *entry;
    packs_download_path(packs, PACKS_PART_SUFFIX, download->part_path, sizeof download->part_path);
    packs_download_path(packs, PACKS_PIECE_SUFFIX, download->chunk_path,
                        sizeof download->chunk_path);
    packs_download_path(packs, PACKS_HASH_SUFFIX, download->state_path,
                        sizeof download->state_path);
    /* The catalog's URLs are relative to the directory the catalog is in. */
    const char *const slash = strrchr(packs->catalog_url, '/');
    const int base = slash != NULL ? (int)(slash - packs->catalog_url + 1) : 0;
    snprintf(download->url, sizeof download->url, "%.*s%s", base, packs->catalog_url, entry->url);

    bool exists = false;
    const uint64_t part_bytes = packs_file_size(download->part_path, &exists);
    if (exists && packs_load_hash(download, part_bytes)) {
        inkwell_log_info("maps", "%s %s: resuming at %" PRIu64 " of %" PRIu64 " bytes", entry->id,
                         entry->cut, download->have, entry->bytes);
    } else {
        (void)remove(download->part_path);
        (void)remove(download->state_path);
        download->have = 0U;
        inkwell_sha256_init(&download->digest);
    }
    (void)remove(download->chunk_path);

    packs->now_ms = now_ms;
    packs_set(packs, MESH_MAP_PACKS_DOWNLOADING, "");
    if (download->have >= entry->bytes) {
        packs_finish(packs);
        return 0;
    }
    packs_next_piece(packs, now_ms);
    return 0;
}

void mesh_map_packs_cancel(struct mesh_map_packs *packs) {
    if (packs == NULL || packs->state != MESH_MAP_PACKS_DOWNLOADING) {
        return;
    }
    inkwell_fetch_cancel(&packs->fetch);
    (void)remove(packs->download.chunk_path);
    packs_set(packs, MESH_MAP_PACKS_READY, inkcell_str(MESH_STR_MAP_PACKS_STOPPED));
}

/* Whether `name` is one of `id`'s files: `<id>.mctp` (a pack named by hand) or `<id>.<cut>`
   followed by one of this module's suffixes. The dot after the id is what keeps `us` from
   matching `us-texas`. */
static bool packs_name_is_id(const char *name, const char *id) {
    const size_t id_len = strlen(id);
    if (strncmp(name, id, id_len) != 0 || name[id_len] != '.') {
        return false;
    }
    const char *const rest = name + id_len;
    if (strcasecmp(rest, PACKS_SUFFIX) == 0) {
        return true;
    }
    char cut[MESH_MAP_PACKS_CUT_MAX];
    if (strlen(rest) < 9U) {
        return false;
    }
    memcpy(cut, rest + 1, 8U);
    cut[8] = '\0';
    const char *const suffix = rest + 9;
    return packs_cut_ok(cut) &&
           (strcasecmp(suffix, PACKS_SUFFIX) == 0 || strcasecmp(suffix, PACKS_PART_SUFFIX) == 0 ||
            strcasecmp(suffix, PACKS_PIECE_SUFFIX) == 0 ||
            strcasecmp(suffix, PACKS_HASH_SUFFIX) == 0);
}

#define PACKS_REMOVE_MAX 16U

struct packs_doomed {
    char names[PACKS_REMOVE_MAX][MESH_MAP_PACKS_ID_MAX + 32U];
    size_t count;
    const char *id;
    const char *keep;
};

static void packs_collect_id(void *context, const char *name) {
    struct packs_doomed *const doomed = context;
    if (doomed->count < PACKS_REMOVE_MAX && packs_name_is_id(name, doomed->id) &&
        (doomed->keep == NULL || strcmp(name, doomed->keep) != 0) &&
        strlen(name) < sizeof doomed->names[0]) {
        snprintf(doomed->names[doomed->count++], sizeof doomed->names[0], "%s", name);
    }
}

/* 0, or the first -errno: a directory that would not list, or a file that would not go. Every
   file is still tried after one fails, so what can be removed is. */
static int packs_remove_id(struct mesh_map_packs *packs, const char *id, const char *keep) {
    /* Collected first and removed after, rather than removed while the directory is being
       read: what readdir() does with an entry deleted under it is the filesystem's business. */
    struct packs_doomed *const doomed = calloc(1U, sizeof *doomed);
    if (doomed == NULL) {
        return -ENOMEM;
    }
    doomed->id = id;
    doomed->keep = keep;
    int result = inkwell_file_list(packs->dir, packs_collect_id, doomed);
    for (size_t i = 0U; i < doomed->count; ++i) {
        char path[MESH_MAP_PACKS_PATH_MAX + 96U];
        snprintf(path, sizeof path, "%s/%s", packs->dir, doomed->names[i]);
        if (remove(path) != 0) {
            const int failed = errno != 0 ? -errno : -EIO;
            inkwell_log_warn("maps", "%s could not be removed: %s", path, strerror(-failed));
            result = result < 0 ? result : failed;
        }
    }
    free(doomed);
    return result < 0 ? result : 0;
}

int mesh_map_packs_delete(struct mesh_map_packs *packs, const char *id) {
    if (packs == NULL || id == NULL || id[0] == '\0') {
        return -EINVAL;
    }
    if (packs->state == MESH_MAP_PACKS_DOWNLOADING && strcmp(packs->download.entry.id, id) == 0) {
        return -EBUSY;
    }
    const struct mesh_map_packs_installed *const installed =
        mesh_map_packs_find_installed(packs, id);
    if (installed == NULL) {
        return -ENOENT;
    }
    const struct mesh_map_packs_entry *const entry = mesh_map_packs_find(packs, id);
    char name[MESH_MAP_PACKS_NAME_MAX + MESH_MAP_PACKS_ID_MAX];
    snprintf(name, sizeof name, "%s", entry != NULL ? entry->name : installed->id);
    const int removed = packs_remove_id(packs, id, NULL);
    /* Rescanned and announced to the map either way: a delete that failed half way has still
       changed the card, and the list says what is there rather than what was asked for. */
    mesh_map_packs_rescan(packs);
    ++packs->installed_revision;
    const bool gone = mesh_map_packs_find_installed(packs, id) == NULL;
    char message[MESH_MAP_PACKS_MESSAGE_MAX];
    inkcell_str_format(
        message, sizeof message,
        removed == 0 && gone ? MESH_STR_MAP_PACKS_DELETED : MESH_STR_MAP_PACKS_DELETE_FAILED, name);
    /* A download of another pack carries on; otherwise the list is what is left to show. */
    const enum mesh_map_packs_state next =
        packs->state == MESH_MAP_PACKS_DOWNLOADING || !packs->catalog_valid ? packs->state
                                                                            : MESH_MAP_PACKS_READY;
    packs_set(packs, next, message);
    if (removed < 0) {
        return removed;
    }
    return gone ? 0 : -EIO;
}
