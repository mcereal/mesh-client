/*
 * Map packs over Wi-Fi: the catalog, the card, and a download in pieces.
 *
 * The catalog and the card are plain data and are tested as such. The download is tested against
 * a real HTTPS server (tests/support/https_fixture.h) rather than a stubbed fetch, because what
 * this module is for is the seams - a piece that stops half way, a resume that has to carry on
 * the digest, a file that must not get the `.mctp` name until it is whole - and none of that
 * exists in a stub.
 */

#include "framework/mesh_test.h"

#include "inkwell/codec/sha256.h"
#include "inkwell/runtime/loop.h"

#include "mesh/core/map_packs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DOWNLOADS_SHA_A "1111111111111111111111111111111111111111111111111111111111111111"

static const char k_catalog[] =
    "{\"format\": 1, \"generated\": \"2026-09-28T12:00:00Z\", \"attribution\": \"x\",\n"
    " \"groups\": [{\"id\": \"north-america\", \"name\": \"North America\", \"parent\": null},\n"
    "            {\"id\": \"us\", \"name\": \"United States\", \"parent\": \"north-america\"}],\n"
    " \"packs\": [\n"
    "  {\"id\": \"world\", \"name\": \"World\", \"parent\": null, \"style\": \"light\",\n"
    "   \"cut\": \"20260927\", \"min_zoom\": 0, \"max_zoom\": 6, \"bbox\": [-85, -180, 85, 180],\n"
    "   \"tiles\": 5461, \"bytes\": 10220240, \"sha256\": \"" DOWNLOADS_SHA_A "\",\n"
    "   \"url\": \"packs/world/20260927-light.mctp\"},\n"
    "  {\"id\": \"us-puerto-rico\", \"name\": \"Puerto Rico\", \"parent\": \"us\",\n"
    "   \"style\": \"dark\", \"cut\": \"20260927\", \"max_zoom\": 14, \"bytes\": 1,\n"
    "   \"sha256\": \"" DOWNLOADS_SHA_A
    "\", \"url\": \"packs/us-puerto-rico/20260927-dark.mctp\"},\n"
    "  {\"id\": \"us-puerto-rico\", \"name\": \"Puerto Rico\", \"parent\": \"us\",\n"
    "   \"style\": \"light\", \"cut\": \"20260927\", \"max_zoom\": 14, \"tiles\": 10285,\n"
    "   \"bytes\": 22481382, \"sha256\": \"" DOWNLOADS_SHA_A "\",\n"
    "   \"url\": \"packs/us-puerto-rico/20260927-light.mctp\", \"new_field\": {\"a\": [1, 2]}},\n"
    "  {\"id\": \"../escape\", \"name\": \"Bad id\", \"cut\": \"20260927\", \"bytes\": 1,\n"
    "   \"sha256\": \"" DOWNLOADS_SHA_A "\", \"url\": \"packs/x.mctp\"},\n"
    "  {\"id\": \"sneaky\", \"name\": \"Bad url\", \"cut\": \"20260927\", \"bytes\": 1,\n"
    "   \"sha256\": \"" DOWNLOADS_SHA_A "\", \"url\": \"../../etc/passwd\"},\n"
    "  {\"id\": \"short\", \"name\": \"Bad digest\", \"cut\": \"20260927\", \"bytes\": 1,\n"
    "   \"sha256\": \"abc\", \"url\": \"packs/short.mctp\"}\n"
    " ]}\n";

/*
 * A catalog is read field by field, keeps what it can trust and drops what it cannot.
 *
 * The three dropped entries are the three ways a catalog could turn into a file this client
 * writes somewhere it should not, or a download it could never verify: an id that is a path, a
 * URL that leaves the catalog's directory, a digest that is not one. A field it does not know
 * (`new_field`) is stepped over, because the document will grow. A style the map does not draw
 * is skipped before it can take the id, so the light Puerto Rico listed after the dark one is
 * the one kept.
 */
MESH_TEST_CASE(map_downloads_catalog_keeps_what_it_can_trust, unit) {
    struct mesh_map_packs_catalog *catalog = calloc(1U, sizeof *catalog);
    MESH_TEST_FAIL_IF(catalog == NULL, "catalog memory");
    const bool parsed = mesh_map_packs_catalog_parse(k_catalog, sizeof k_catalog - 1U, catalog);
    const bool groups = catalog->group_count == 2U && strcmp(catalog->groups[0].parent, "") == 0 &&
                        strcmp(catalog->groups[1].parent, "north-america") == 0;
    const bool entries =
        catalog->entry_count == 2U && strcmp(catalog->entries[0].id, "world") == 0 &&
        strcmp(catalog->entries[1].id, "us-puerto-rico") == 0 &&
        catalog->entries[1].bytes == 22481382U && catalog->entries[1].max_zoom == 14U &&
        strcmp(catalog->entries[1].parent, "us") == 0 &&
        strcmp(catalog->entries[0].parent, "") == 0;

    static const char k_future[] = "{\"format\": 2, \"groups\": [], \"packs\": []}";
    const bool future = mesh_map_packs_catalog_parse(k_future, sizeof k_future - 1U, catalog);
    static const char k_broken[] = "{\"format\": 1, \"packs\": [{\"id\": ";
    const bool broken = mesh_map_packs_catalog_parse(k_broken, sizeof k_broken - 1U, catalog);
    free(catalog);

    MESH_TEST_FAIL_IF(!parsed, "a format-1 catalog parses");
    MESH_TEST_FAIL_IF(!groups, "with its groups, a null parent read as the top");
    MESH_TEST_FAIL_IF(!entries, "and the two packs it can trust, and only those");
    MESH_TEST_FAIL_IF(future, "a format this client does not know is refused");
    MESH_TEST_FAIL_IF(broken, "and so is a document cut short");
    record_success(test_name);
}

/* Appends `piece` to `text` at `*len`, or leaves both alone and returns false when it will not
   fit. */
static bool downloads_append(char *text, size_t cap, size_t *len, const char *piece) {
    const size_t add = strlen(piece);
    if (add >= cap - *len) {
        return false;
    }
    memcpy(text + *len, piece, add + 1U);
    *len += add;
    return true;
}

/* A catalog of every country and the subdivisions of the largest - about 320 packs, which is
   what meshclient-maps plans - is read whole, not cut off at an old limit. */
MESH_TEST_CASE(map_downloads_catalog_holds_the_whole_world, unit) {
    enum { PACKS = 400 };
    const size_t cap = 256U + (size_t)PACKS * 256U;
    char *const text = malloc(cap);
    struct mesh_map_packs_catalog *catalog = calloc(1U, sizeof *catalog);
    MESH_TEST_FAIL_IF(text == NULL || catalog == NULL, "catalog memory");
    size_t len = 0U;
    bool fits = downloads_append(text, cap, &len,
                                 "{\"format\": 1, \"groups\": [{\"id\": \"all\", \"name\": "
                                 "\"All\"}], \"packs\": [");
    for (int i = 0; fits && i < PACKS; ++i) {
        char entry[256];
        snprintf(entry, sizeof entry,
                 "%s{\"id\": \"region-%d\", \"name\": \"Region %d\", \"parent\": \"all\", "
                 "\"style\": \"light\", \"cut\": \"20260927\", \"bytes\": 1, \"sha256\": "
                 "\"" DOWNLOADS_SHA_A "\", \"url\": \"packs/region-%d/20260927-light.mctp\"}",
                 i == 0 ? "" : ",", i, i, i);
        fits = downloads_append(text, cap, &len, entry);
    }
    fits = fits && downloads_append(text, cap, &len, "]}");
    const bool parsed = fits && mesh_map_packs_catalog_parse(text, len, catalog);
    const size_t count = catalog->entry_count;
    const bool last = count == PACKS && strcmp(catalog->entries[PACKS - 1].id, "region-399") == 0;
    free(catalog);
    free(text);

    MESH_TEST_FAIL_IF(!fits, "the catalog is written");
    MESH_TEST_FAIL_IF(!parsed, "and parses");
    MESH_TEST_FAIL_IF(len > MESH_MAP_PACKS_CATALOG_MAX, "inside what a fetch will take");
    MESH_TEST_FAIL_IF(!last, "with every pack in it");
    record_success(test_name);
}

static bool downloads_touch(const char *dir, const char *name, size_t bytes) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *const file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    for (size_t i = 0U; i < bytes; ++i) {
        (void)fputc('x', file);
    }
    return fclose(file) == 0;
}

static bool downloads_exists(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return access(path, F_OK) == 0;
}

/* Removes a scratch directory of flat files. */
static void downloads_clear(const char *dir) {
    char command[600];
    snprintf(command, sizeof command, "rm -rf '%s'", dir);
    const int status = system(command);
    (void)status;
}

/*
 * The card is the record of what is installed, read by name.
 *
 * `<id>.<cut>.mctp` is a region at a cut; any other `.mctp` is a pack copied on by hand and is
 * listed under its own name; a `.part` is a download that has not finished and is not installed.
 * Deleting a region takes every file of it - the stopped download too - and nothing of a region
 * whose id merely starts the same way.
 */
MESH_TEST_CASE(map_downloads_the_card_says_what_is_installed, unit) {
    char dir[] = "/tmp/meshclient_packs_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "a scratch directory");
    const bool laid_out = downloads_touch(dir, "us.20260927.mctp", 10U) &&
                          downloads_touch(dir, "us.20260927.mctp.part", 4U) &&
                          downloads_touch(dir, "us.20260927.mctp.hash", 4U) &&
                          downloads_touch(dir, "us-texas.20260927.mctp", 20U) &&
                          downloads_touch(dir, "Home.MCTP", 30U) &&
                          downloads_touch(dir, "._Home.MCTP", 1U) &&
                          downloads_touch(dir, "half.20260927.mctp.part", 1U);

    struct mesh_map_packs *packs = calloc(1U, sizeof *packs);
    MESH_TEST_FAIL_IF_CLEANUP(packs == NULL, downloads_clear(dir), "module memory");
    (void)mesh_map_packs_init(packs, NULL, dir);
    const bool listed = packs->installed_count == 3U &&
                        strcmp(packs->installed[0].id, "Home") == 0 &&
                        packs->installed[0].cut[0] == '\0' && packs->installed[0].bytes == 30U &&
                        strcmp(packs->installed[1].id, "us") == 0 &&
                        strcmp(packs->installed[1].cut, "20260927") == 0 &&
                        strcmp(packs->installed[2].id, "us-texas") == 0;
    const uint32_t before = packs->installed_revision;
    const int deleted = mesh_map_packs_delete(packs, "us");
    const bool gone = !downloads_exists(dir, "us.20260927.mctp") &&
                      !downloads_exists(dir, "us.20260927.mctp.part") &&
                      !downloads_exists(dir, "us.20260927.mctp.hash");
    const bool kept = downloads_exists(dir, "us-texas.20260927.mctp") &&
                      packs->installed_count == 2U && packs->installed_revision != before;
    const int again = mesh_map_packs_delete(packs, "us");
    const int by_hand = mesh_map_packs_delete(packs, "Home");
    const bool hand_gone = !downloads_exists(dir, "Home.MCTP");
    const bool no_fetch =
        !mesh_map_packs_available(packs) && mesh_map_packs_refresh(packs, 0U) == -ENOTSUP;
    mesh_map_packs_shutdown(packs);
    free(packs);
    downloads_clear(dir);

    MESH_TEST_FAIL_IF(!laid_out, "the card is laid out");
    MESH_TEST_FAIL_IF(!listed, "the packs are listed by id and cut, the .part is not");
    MESH_TEST_FAIL_IF(deleted != 0 || !gone, "deleting a region takes all of its files");
    MESH_TEST_FAIL_IF(!kept, "and only its files, and the map is told");
    MESH_TEST_FAIL_IF(again != -ENOENT, "a region deleted twice is not there the second time");
    MESH_TEST_FAIL_IF(by_hand != 0 || !hand_gone, "a pack copied on by hand deletes by its name");
    MESH_TEST_FAIL_IF(!no_fetch, "with no loop there is nothing to fetch with");
    record_success(test_name);
}

/*
 * The legacy single pack the map opens beside the directory takes a slot of the same stack, so
 * it counts against what a new region may fill - and only while it is there.
 */
MESH_TEST_CASE(map_downloads_count_the_pack_beside_the_directory, unit) {
    char dir[] = "/tmp/meshclient_packs_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "a scratch directory");
    char maps[64];
    snprintf(maps, sizeof maps, "%s/maps", dir);
    char legacy[64];
    snprintf(legacy, sizeof legacy, "%s/map.mctp", dir);
    const bool laid_out = mkdir(maps, 0700) == 0 && downloads_touch(maps, "us.20260927.mctp", 10U);

    struct mesh_map_packs *packs = calloc(1U, sizeof *packs);
    MESH_TEST_FAIL_IF_CLEANUP(packs == NULL, downloads_clear(dir), "module memory");
    (void)mesh_map_packs_init(packs, NULL, maps);
    mesh_map_packs_count_also(packs, legacy);
    const bool absent = packs->on_card == 1U;
    const bool legacy_laid = downloads_touch(dir, "map.mctp", 10U);
    mesh_map_packs_rescan(packs);
    const bool present = packs->on_card == 2U && packs->installed_count == 1U;
    mesh_map_packs_shutdown(packs);
    free(packs);
    downloads_clear(dir);

    MESH_TEST_FAIL_IF(!laid_out || !legacy_laid, "the card is laid out");
    MESH_TEST_FAIL_IF(!absent, "a legacy pack that is not there counts nothing");
    MESH_TEST_FAIL_IF(!present, "one that is there counts, and is not listed as a region");
    record_success(test_name);
}

/*
 * With no directory the module manages nothing: that is how the app says the map is drawing a
 * single file MESHCLIENT_MAP_PACK named, which no download could reach. It lists nothing and
 * fetches nothing, rather than filling a directory the map is not reading.
 */
MESH_TEST_CASE(map_downloads_without_a_directory_download_nothing, unit) {
    struct inkwell_loop loop;
    MESH_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "event loop init failed");
    struct mesh_map_packs *packs = calloc(1U, sizeof *packs);
    MESH_TEST_FAIL_IF_CLEANUP(packs == NULL, inkwell_loop_shutdown(&loop), "module memory");
    (void)mesh_map_packs_init(packs, &loop, "");
    const bool off = !mesh_map_packs_available(packs) && packs->installed_count == 0U &&
                     mesh_map_packs_refresh(packs, 0U) == -ENOTSUP &&
                     mesh_map_packs_download(packs, "world", 0U) == -ENOTSUP;
    mesh_map_packs_shutdown(packs);
    free(packs);
    inkwell_loop_shutdown(&loop);
    MESH_TEST_FAIL_IF(!off, "a module with no directory offers nothing");
    record_success(test_name);
}

/*
 * A delete the card refuses says so. Reporting it deleted would leave a pack the map still
 * draws under a line saying it is gone, and the reader no reason to look for why.
 */
MESH_TEST_CASE(map_downloads_a_refused_delete_is_not_a_delete, unit) {
    if (geteuid() == 0) {
        /* root removes from a read-only directory, so there is no refusal to provoke. */
        record_success(test_name);
        return;
    }
    char dir[] = "/tmp/meshclient_packs_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "a scratch directory");
    const bool laid_out = downloads_touch(dir, "world.20260927.mctp", 10U);
    struct mesh_map_packs *packs = calloc(1U, sizeof *packs);
    MESH_TEST_FAIL_IF_CLEANUP(packs == NULL, downloads_clear(dir), "module memory");
    (void)mesh_map_packs_init(packs, NULL, dir);
    (void)chmod(dir, 0500);
    const int deleted = mesh_map_packs_delete(packs, "world");
    const bool still_listed = mesh_map_packs_find_installed(packs, "world") != NULL;
    const bool said_so = strstr(packs->message, "could not be deleted") != NULL;
    (void)chmod(dir, 0700);
    mesh_map_packs_shutdown(packs);
    free(packs);
    downloads_clear(dir);
    MESH_TEST_FAIL_IF(!laid_out, "the card is laid out");
    MESH_TEST_FAIL_IF(deleted >= 0, "a delete the card refused reports the refusal");
    MESH_TEST_FAIL_IF(!still_listed, "and the pack is still listed, because it is still there");
    MESH_TEST_FAIL_IF(!said_so, "and the status says it could not be deleted");
    record_success(test_name);
}

#ifdef INKWELL_HAVE_TLS

#include "support/https_fixture.h"

/* Nine and a bit megabytes: three pieces, the last one short. */
#define DOWNLOADS_PACK_BYTES (2U * MESH_MAP_PACKS_CHUNK_BYTES + 777777U)

struct downloads_server {
    char catalog[256];
    char pack[256];
    /* While this file exists, every piece but the first is cut off part way - a network that
       dropped after the first piece. */
    char cut_gate[256];
};

static void downloads_serve(void *userdata, const struct https_fixture_request *request,
                            struct https_fixture_conn *conn) {
    const struct downloads_server *const server = userdata;
    if (strcmp(request->target, "/v1/catalog.json") == 0) {
        https_fixture_reply_file(conn, request, server->catalog);
        return;
    }
    if (strcmp(request->target, "/v1/packs/test-region/20260927-light.mctp") != 0) {
        https_fixture_reply(conn, 404, NULL, NULL, 0U);
        return;
    }
    if (request->ranged && request->first > 0U && access(server->cut_gate, F_OK) == 0) {
        https_fixture_printf(conn,
                             "HTTP/1.1 206 Partial Content\r\nContent-Length: %llu\r\n"
                             "Content-Range: bytes %llu-%llu/%u\r\n\r\n",
                             (unsigned long long)(request->last - request->first + 1U),
                             (unsigned long long)request->first, (unsigned long long)request->last,
                             DOWNLOADS_PACK_BYTES);
        static const char k_some[1000] = {0};
        https_fixture_send(conn, k_some, sizeof k_some);
        https_fixture_cut(conn);
        return;
    }
    https_fixture_reply_file(conn, request, server->pack);
}

struct downloads_rig {
    char dir[64];
    char maps[128];
    struct downloads_server server_files;
    struct https_fixture server;
    struct inkwell_loop loop;
    struct mesh_map_packs packs;
    bool loop_up;
    bool packs_up;
    char sha[65];
};

/* A pack of deterministic bytes, and a catalog offering it with `sha` as its digest. */
static bool downloads_write_fixtures(struct downloads_rig *rig, const char *sha) {
    FILE *pack = fopen(rig->server_files.pack, "wb");
    if (pack == NULL) {
        return false;
    }
    uint32_t state = 12345U;
    for (uint32_t i = 0U; i < DOWNLOADS_PACK_BYTES; ++i) {
        state = state * 1103515245U + 12345U;
        (void)fputc((int)(state >> 24), pack);
    }
    if (fclose(pack) != 0) {
        return false;
    }
    uint8_t digest[32];
    if (inkwell_sha256_file(rig->server_files.pack, digest) != 0) {
        return false;
    }
    inkwell_sha256_hex(digest, rig->sha, sizeof rig->sha);
    FILE *catalog = fopen(rig->server_files.catalog, "wb");
    if (catalog == NULL) {
        return false;
    }
    fprintf(catalog,
            "{\"format\": 1, \"groups\": [], \"packs\": [{\"id\": \"test-region\", \"name\": "
            "\"Test Region\", \"parent\": null, \"cut\": \"20260927\", \"max_zoom\": 13, "
            "\"bytes\": %u, \"sha256\": \"%s\", "
            "\"url\": \"packs/test-region/20260927-light.mctp\"}]}",
            DOWNLOADS_PACK_BYTES, sha != NULL ? sha : rig->sha);
    return fclose(catalog) == 0;
}

static const char *downloads_rig_up(struct downloads_rig *rig, const char *sha) {
    memset(rig, 0, sizeof *rig);
    snprintf(rig->dir, sizeof rig->dir, "/tmp/meshclient_dl_XXXXXX");
    if (mkdtemp(rig->dir) == NULL) {
        return "a scratch directory";
    }
    snprintf(rig->maps, sizeof rig->maps, "%s/home/.meshclient/maps", rig->dir);
    snprintf(rig->server_files.catalog, sizeof rig->server_files.catalog, "%s/catalog.json",
             rig->dir);
    snprintf(rig->server_files.pack, sizeof rig->server_files.pack, "%s/pack.bin", rig->dir);
    snprintf(rig->server_files.cut_gate, sizeof rig->server_files.cut_gate, "%s/cut", rig->dir);
    char home[160];
    snprintf(home, sizeof home, "%s/home", rig->dir);
    if (mkdir(home, 0700) != 0 || !downloads_write_fixtures(rig, sha)) {
        return "the fixtures could not be written";
    }
    if (!https_fixture_start(&rig->server, downloads_serve, &rig->server_files)) {
        return "the fake map server would not start";
    }
    if (inkwell_loop_init(&rig->loop) != 0) {
        return "event loop init failed";
    }
    rig->loop_up = true;
    if (mesh_map_packs_init(&rig->packs, &rig->loop, rig->maps) != 0) {
        return "module init failed";
    }
    rig->packs_up = true;
    https_fixture_attach(&rig->server, &rig->packs.fetch);
    mesh_map_packs_set_catalog_url(&rig->packs, "https://example.invalid/v1/catalog.json");
    return NULL;
}

static void downloads_rig_down(struct downloads_rig *rig) {
    if (rig->packs_up) {
        mesh_map_packs_shutdown(&rig->packs);
    }
    if (rig->loop_up) {
        inkwell_loop_shutdown(&rig->loop);
    }
    https_fixture_stop(&rig->server);
    downloads_clear(rig->dir);
}

/* Pumps the loop until the module leaves `from`. */
static bool downloads_wait_past(struct downloads_rig *rig, enum mesh_map_packs_state from) {
    for (int i = 0; i < 600 && rig->packs.state == from; ++i) {
        inkwell_loop_run(&rig->loop, 50);
        mesh_map_packs_tick(&rig->packs, (uint64_t)i * 50U);
    }
    return rig->packs.state != from;
}

static bool downloads_installed_whole(const struct downloads_rig *rig) {
    char path[256];
    snprintf(path, sizeof path, "%s/test-region.20260927.mctp", rig->maps);
    uint8_t digest[32];
    char hex[65];
    if (inkwell_sha256_file(path, digest) != 0) {
        return false;
    }
    inkwell_sha256_hex(digest, hex, sizeof hex);
    char part[sizeof path + sizeof ".part"];
    snprintf(part, sizeof part, "%s.part", path);
    return strcmp(hex, rig->sha) == 0 && access(part, F_OK) != 0;
}

/*
 * The catalog, then a pack in three pieces, verified and renamed into place - and the older cut
 * of the same region, which was there first, gone once the new one is.
 */
MESH_TEST_CASE(map_downloads_fetch_a_pack_in_pieces, unit) {
    struct downloads_rig rig;
    const char *failure = downloads_rig_up(&rig, NULL);
    if (failure == NULL) {
        char parent[160];
        snprintf(parent, sizeof parent, "%s/home/.meshclient", rig.dir);
        (void)mkdir(parent, 0700);
        (void)mkdir(rig.maps, 0700);
        if (!downloads_touch(rig.maps, "test-region.20250101.mctp", 5U)) {
            failure = "the older cut could not be written";
        }
        mesh_map_packs_rescan(&rig.packs);
    }
    if (failure == NULL &&
        (mesh_map_packs_refresh(&rig.packs, 0U) != 0 ||
         !downloads_wait_past(&rig, MESH_MAP_PACKS_LOADING) ||
         rig.packs.state != MESH_MAP_PACKS_READY || rig.packs.catalog.entry_count != 1U)) {
        failure = "the catalog did not arrive";
    }
    const uint32_t before = rig.packs.installed_revision;
    if (failure == NULL && mesh_map_packs_download(&rig.packs, "test-region", 0U) != 0) {
        failure = "the download would not start";
    }
    if (failure == NULL && !mesh_map_packs_holds_the_antenna(&rig.packs)) {
        failure = "a download does not hold the antenna";
    }
    if (failure == NULL && (!downloads_wait_past(&rig, MESH_MAP_PACKS_DOWNLOADING) ||
                            rig.packs.state != MESH_MAP_PACKS_READY)) {
        failure = rig.packs.message[0] != '\0' ? rig.packs.message : "the download did not finish";
    }
    if (failure == NULL && !downloads_installed_whole(&rig)) {
        failure = "the installed pack is not the file served";
    }
    if (failure == NULL &&
        (downloads_exists(rig.maps, "test-region.20250101.mctp") ||
         rig.packs.installed_count != 1U || rig.packs.installed_revision == before ||
         strcmp(rig.packs.installed[0].cut, "20260927") != 0)) {
        failure = "the older cut was not replaced";
    }
    char log[4096] = {0};
    (void)https_fixture_requests(&rig.server, log, sizeof log);
    if (failure == NULL && strstr(log, " 8388608-") == NULL) {
        failure = "the pack did not come down in pieces";
    }
    downloads_rig_down(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A download cut off after its first piece resumes from the second, not from the start, and
 * still verifies - which it can only do because the digest of the first piece was kept.
 */
MESH_TEST_CASE(map_downloads_resume_where_they_stopped, unit) {
    struct downloads_rig rig;
    const char *failure = downloads_rig_up(&rig, NULL);
    if (failure == NULL) {
        FILE *const gate = fopen(rig.server_files.cut_gate, "wb");
        if (gate == NULL || fclose(gate) != 0) {
            failure = "the gate could not be set";
        }
    }
    if (failure == NULL && (mesh_map_packs_refresh(&rig.packs, 0U) != 0 ||
                            !downloads_wait_past(&rig, MESH_MAP_PACKS_LOADING))) {
        failure = "the catalog did not arrive";
    }
    if (failure == NULL && (mesh_map_packs_download(&rig.packs, "test-region", 0U) != 0 ||
                            !downloads_wait_past(&rig, MESH_MAP_PACKS_DOWNLOADING) ||
                            rig.packs.state != MESH_MAP_PACKS_FAILED)) {
        failure = "the cut-off download did not fail";
    }
    char part[256];
    snprintf(part, sizeof part, "%s/test-region.20260927.mctp.part", rig.maps);
    struct stat info;
    if (failure == NULL &&
        (stat(part, &info) != 0 || (uint64_t)info.st_size != MESH_MAP_PACKS_CHUNK_BYTES)) {
        failure = "the first piece was not kept";
    }
    (void)remove(rig.server_files.cut_gate);
    if (failure == NULL && (mesh_map_packs_download(&rig.packs, "test-region", 0U) != 0 ||
                            !downloads_wait_past(&rig, MESH_MAP_PACKS_DOWNLOADING) ||
                            rig.packs.state != MESH_MAP_PACKS_READY)) {
        failure = rig.packs.message[0] != '\0' ? rig.packs.message : "the resume did not finish";
    }
    if (failure == NULL && !downloads_installed_whole(&rig)) {
        failure = "the resumed pack is not the file served";
    }
    char log[4096] = {0};
    (void)https_fixture_requests(&rig.server, log, sizeof log);
    /* Two ranged requests from zero would mean the resume started again. */
    const char *const first = strstr(log, " 0-");
    if (failure == NULL && (first == NULL || strstr(first + 1, " 0-") != NULL)) {
        failure = "the resume asked for the first piece again";
    }
    downloads_rig_down(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A pack whose bytes do not hash to the catalog's digest is thrown away whole, never installed. */
MESH_TEST_CASE(map_downloads_refuse_a_pack_that_does_not_verify, unit) {
    struct downloads_rig rig;
    const char *failure = downloads_rig_up(&rig, DOWNLOADS_SHA_A);
    if (failure == NULL && (mesh_map_packs_refresh(&rig.packs, 0U) != 0 ||
                            !downloads_wait_past(&rig, MESH_MAP_PACKS_LOADING))) {
        failure = "the catalog did not arrive";
    }
    if (failure == NULL && (mesh_map_packs_download(&rig.packs, "test-region", 0U) != 0 ||
                            !downloads_wait_past(&rig, MESH_MAP_PACKS_DOWNLOADING) ||
                            rig.packs.state != MESH_MAP_PACKS_FAILED)) {
        failure = "a pack that does not verify did not fail";
    }
    if (failure == NULL && (downloads_exists(rig.maps, "test-region.20260927.mctp") ||
                            downloads_exists(rig.maps, "test-region.20260927.mctp.part") ||
                            rig.packs.installed_count != 0U)) {
        failure = "what did not verify was kept";
    }
    downloads_rig_down(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The map draws MESH_MAP_PACKS_DRAWN_MAX packs, so with that many installed a new region is
 * refused - the map would leave it out, or push out one it was drawing, and the section would
 * have said "on the map" of it. A newer cut of a region already installed replaces it, so it is
 * not refused.
 */
MESH_TEST_CASE(map_downloads_stop_at_what_the_map_draws, unit) {
    struct downloads_rig rig;
    const char *failure = downloads_rig_up(&rig, NULL);
    if (failure == NULL) {
        char parent[160];
        snprintf(parent, sizeof parent, "%s/home/.meshclient", rig.dir);
        (void)mkdir(parent, 0700);
        (void)mkdir(rig.maps, 0700);
        for (unsigned i = 0U; i < MESH_MAP_PACKS_DRAWN_MAX && failure == NULL; ++i) {
            char name[128];
            /* The last is a pack copied on by hand under a name too long to list, which the
               map draws all the same and so counts. */
            if (i + 1U == MESH_MAP_PACKS_DRAWN_MAX) {
                snprintf(name, sizeof name, "%s",
                         "a-pack-somebody-copied-on-by-hand-with-a-very-long-descriptive-"
                         "name.mctp");
            } else {
                snprintf(name, sizeof name, "region-%02u.20260927.mctp", i);
            }
            if (!downloads_touch(rig.maps, name, 1U)) {
                failure = "the full card could not be laid out";
            }
        }
        mesh_map_packs_rescan(&rig.packs);
    }
    if (failure == NULL && (mesh_map_packs_refresh(&rig.packs, 0U) != 0 ||
                            !downloads_wait_past(&rig, MESH_MAP_PACKS_LOADING))) {
        failure = "the catalog did not arrive";
    }
    if (failure == NULL && rig.packs.installed_count != MESH_MAP_PACKS_DRAWN_MAX - 1U) {
        failure = "the long name should be counted but not listed";
    }
    if (failure == NULL && mesh_map_packs_download(&rig.packs, "test-region", 0U) != -ENOSPC) {
        failure = "a region past what the map draws was not refused";
    }
    if (failure == NULL) {
        /* One of the sixteen becomes an older cut of the region on offer. */
        char from[256];
        char to[256];
        snprintf(from, sizeof from, "%s/region-00.20260927.mctp", rig.maps);
        snprintf(to, sizeof to, "%s/test-region.20250101.mctp", rig.maps);
        if (rename(from, to) != 0) {
            failure = "the older cut could not be put in place";
        }
        mesh_map_packs_rescan(&rig.packs);
    }
    if (failure == NULL && (mesh_map_packs_download(&rig.packs, "test-region", 0U) != 0 ||
                            !downloads_wait_past(&rig, MESH_MAP_PACKS_DOWNLOADING) ||
                            rig.packs.state != MESH_MAP_PACKS_READY ||
                            rig.packs.on_card != MESH_MAP_PACKS_DRAWN_MAX)) {
        failure = "an update of an installed region was refused, or grew the set";
    }
    downloads_rig_down(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

#endif /* INKWELL_HAVE_TLS */
