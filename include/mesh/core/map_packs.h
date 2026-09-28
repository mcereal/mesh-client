#pragma once

/*
 * Map packs over Wi-Fi: the catalog of regions meshclient-maps publishes, the packs installed on
 * this card, and one download at a time from the first into the second.
 *
 * The catalog is https://maps.sailfin.dev/v1/catalog.json; its contract is meshclient-maps'
 * docs/catalog.md, and the one rule that shapes everything here is that a pack's URL names a cut
 * and never changes, so the bytes behind it never do either. That is what makes a download
 * resumable: an interrupted one continues from the bytes it has, with a Range request, knowing
 * they are the start of the same file.
 *
 * **A pack comes down in pieces.** Each piece is one Range request of MESH_MAP_PACKS_CHUNK_BYTES
 * into a file of its own - inkwell's fetch writes a fresh file per request - and is then
 * appended to `<id>.<cut>.mctp.part` and hashed on the way. So the digest of a 200 MB file is
 * built up a piece at a time rather than taken in one read that would stall the one loop this
 * client has for seconds, and an interruption loses at most one piece. The digest so far is
 * saved beside the `.part` after every piece, which is what lets a resume carry on hashing
 * rather than reading back everything it already has.
 *
 * **Only a whole, verified file gets the `.mctp` name.** The `.part` is renamed into place once
 * its length and SHA-256 match the catalog's; the map (mesh/map/stack.h) opens every `*.mctp`
 * in the directory and never a `.part`, so it cannot draw half a download. The older cut of the
 * same region is deleted after the new one is in place, not before.
 *
 * The directory is the source of truth for what is installed: a file named `<id>.<cut>.mctp` is
 * that region at that cut, and any other `*.mctp` is a pack somebody copied on by hand, listed
 * under its own name. Nothing else records what is installed, so nothing can disagree with the
 * card.
 *
 * Wi-Fi and Bluetooth share one antenna on the device, so a download holds it - see
 * mesh_map_packs_holds_the_antenna() - exactly as the self-updater's does.
 */

#include "inkwell/codec/sha256.h"
#include "inkwell/net/fetch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

#define MESH_MAP_PACKS_CATALOG_URL "https://maps.sailfin.dev/v1/catalog.json"

/* What this client reads. A catalog of another format is refused rather than guessed at. */
#define MESH_MAP_PACKS_FORMAT 1U

/* One piece of a download: a Range request's worth. Small enough that appending it is a pause
   the loop does not notice, large enough that the request overhead is noise. */
#define MESH_MAP_PACKS_CHUNK_BYTES (4U * 1024U * 1024U)

/* The catalog is a few KB per hundred regions; this is room for thousands and a cap on a
   reply that is not what it claims to be. */
#define MESH_MAP_PACKS_CATALOG_MAX (512U * 1024U)

#define MESH_MAP_PACKS_GROUPS_MAX 32U
#define MESH_MAP_PACKS_ENTRIES_MAX 96U
#define MESH_MAP_PACKS_INSTALLED_MAX 32U

#define MESH_MAP_PACKS_ID_MAX 48U
/* The pack header's name field is 32 bytes; the catalog promises at most 31 of UTF-8. */
#define MESH_MAP_PACKS_NAME_MAX 32U
#define MESH_MAP_PACKS_CUT_MAX 9U /* YYYYMMDD */
#define MESH_MAP_PACKS_URL_MAX 192U
#define MESH_MAP_PACKS_PATH_MAX 512U
#define MESH_MAP_PACKS_MESSAGE_MAX 96U

enum mesh_map_packs_state {
    MESH_MAP_PACKS_IDLE = 0,    /* the catalog has not been asked for this run */
    MESH_MAP_PACKS_LOADING,     /* asking for the catalog */
    MESH_MAP_PACKS_READY,       /* the catalog is held; `message` may say what just happened */
    MESH_MAP_PACKS_DOWNLOADING, /* `download` says which and how far */
    MESH_MAP_PACKS_FAILED,      /* `message` says why; the catalog may still be held */
    MESH_MAP_PACKS_STATE_COUNT,
};

/* A heading in the catalog's tree. */
struct mesh_map_packs_group {
    char id[MESH_MAP_PACKS_ID_MAX];
    char name[MESH_MAP_PACKS_NAME_MAX];
    char parent[MESH_MAP_PACKS_ID_MAX]; /* "" at the top */
};

/* One download the catalog offers. */
struct mesh_map_packs_entry {
    char id[MESH_MAP_PACKS_ID_MAX];
    char name[MESH_MAP_PACKS_NAME_MAX];
    char parent[MESH_MAP_PACKS_ID_MAX]; /* a group id, or "" for the top (the world base) */
    char cut[MESH_MAP_PACKS_CUT_MAX];
    char sha256[65];
    char url[MESH_MAP_PACKS_URL_MAX]; /* relative to the catalog's directory */
    uint64_t bytes;
    uint32_t tiles;
    uint8_t max_zoom;
};

struct mesh_map_packs_catalog {
    struct mesh_map_packs_group groups[MESH_MAP_PACKS_GROUPS_MAX];
    size_t group_count;
    struct mesh_map_packs_entry entries[MESH_MAP_PACKS_ENTRIES_MAX];
    size_t entry_count;
};

/* A pack on the card. `cut` is "" for a file not named `<id>.<cut>.mctp`, which is then listed
   under `id` = its file name without the extension. */
struct mesh_map_packs_installed {
    char id[MESH_MAP_PACKS_ID_MAX];
    char cut[MESH_MAP_PACKS_CUT_MAX];
    char file[MESH_MAP_PACKS_ID_MAX + MESH_MAP_PACKS_CUT_MAX + 8U];
    uint64_t bytes;
};

struct mesh_map_packs_download {
    struct mesh_map_packs_entry entry;
    uint64_t have; /* bytes verified into the .part */
    struct inkwell_sha256 digest;
    char part_path[MESH_MAP_PACKS_PATH_MAX];
    char chunk_path[MESH_MAP_PACKS_PATH_MAX];
    char state_path[MESH_MAP_PACKS_PATH_MAX];
    char url[MESH_MAP_PACKS_URL_MAX + 64U];
    char range[64];
    uint64_t chunk_len;
};

struct mesh_map_packs {
    struct inkwell_fetch fetch;
    enum mesh_map_packs_state state;
    char dir[MESH_MAP_PACKS_PATH_MAX];
    char catalog_url[MESH_MAP_PACKS_URL_MAX];
    struct mesh_map_packs_catalog catalog;
    bool catalog_valid;
    struct mesh_map_packs_installed installed[MESH_MAP_PACKS_INSTALLED_MAX];
    size_t installed_count;
    struct mesh_map_packs_download download;
    char message[MESH_MAP_PACKS_MESSAGE_MAX];
    /* Changes whenever anything a screen shows does. */
    uint32_t revision;
    /* Changes only when the set of files the map draws does: an install or a delete. */
    uint32_t installed_revision;
    /* The loop's clock as of the last tick or start, for the piece a finished one starts. */
    uint64_t now_ms;
};

/*
 * Ready to use, with nothing fetched. `dir` is where packs live (created on the first
 * download); `loop` may be NULL, which makes the module list what is installed and refuse to
 * fetch - a build with no TLS behaves the same way. 0, or -errno.
 */
int mesh_map_packs_init(struct mesh_map_packs *packs, struct inkwell_loop *loop, const char *dir);

/* Abandons a download in flight (its .part stays, for a resume) and releases everything. */
void mesh_map_packs_shutdown(struct mesh_map_packs *packs);

/* Whether this build and this loop can fetch at all. */
bool mesh_map_packs_available(const struct mesh_map_packs *packs);

/* Enforces the fetch's deadlines. Called every turn of the loop, as the updater's tick is. */
void mesh_map_packs_tick(struct mesh_map_packs *packs, uint64_t now_ms);

/* Asks for the catalog. -EBUSY while a request runs, -ENOTSUP when fetching is unavailable. */
int mesh_map_packs_refresh(struct mesh_map_packs *packs, uint64_t now_ms);

/*
 * Downloads the catalog's pack `id`, resuming a `.part` of the same cut if there is one.
 * -ENOENT when the catalog has no such pack, -EBUSY while a request runs, -ENOTSUP when
 * fetching is unavailable, or -errno from the card.
 */
int mesh_map_packs_download(struct mesh_map_packs *packs, const char *id, uint64_t now_ms);

/* Stops the download in flight. What has arrived is kept for the next attempt. */
void mesh_map_packs_cancel(struct mesh_map_packs *packs);

/*
 * Deletes every installed file of `id` - and a stopped download of it. -EBUSY while that pack is
 * downloading, -ENOENT when nothing of it is installed.
 */
int mesh_map_packs_delete(struct mesh_map_packs *packs, const char *id);

/* Re-reads the directory. Called by init and after every change this module makes; a caller
   needs it only when something else may have written the card. */
void mesh_map_packs_rescan(struct mesh_map_packs *packs);

/* The catalog entry for `id`, or NULL. */
const struct mesh_map_packs_entry *mesh_map_packs_find(const struct mesh_map_packs *packs,
                                                       const char *id);

/* The installed pack `id`, or NULL. */
const struct mesh_map_packs_installed *
mesh_map_packs_find_installed(const struct mesh_map_packs *packs, const char *id);

/* How far the download is, in permille; false when none is running. */
bool mesh_map_packs_progress(const struct mesh_map_packs *packs, uint16_t *permille);

/* True while a download needs the antenna. */
bool mesh_map_packs_holds_the_antenna(const struct mesh_map_packs *packs);

/* Parses a catalog document into `out`. False for anything but a format-1 catalog; entries with
   a field this client would not trust (an id it could not name a file after, a URL that leaves
   the catalog's directory, a digest that is not one) are dropped rather than failing the lot. */
bool mesh_map_packs_catalog_parse(const char *text, size_t len, struct mesh_map_packs_catalog *out);

/* The state's name, for a log line. */
const char *mesh_map_packs_state_name(enum mesh_map_packs_state state);

/* Points the fetcher at a test server instead of the URL's host. */
void mesh_map_packs_connect_to(struct mesh_map_packs *packs, const char *host, uint16_t port);

/* Asks for the catalog at another URL, for a test. */
void mesh_map_packs_set_catalog_url(struct mesh_map_packs *packs, const char *url);

#ifdef __cplusplus
}
#endif
