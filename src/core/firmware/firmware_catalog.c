#include "mesh/core/firmware_catalog.h"

#include "mesh/utils/json.h"
#include "mesh/utils/text.h"

#include <stdlib.h>
#include <string.h>

/*
 * The architectures upstream's hardware document spells, and what each one means for us.
 *
 * Spelled out as a table rather than derived from a prefix, because the prefixes lie in both
 * directions: "esp32-s3" and "esp32" take the same path while "esp32-c3" - which starts with
 * the same six characters - takes none, and "rp2040" shares nothing textual with "nrf52840"
 * although the two are the same UF2 write with a different family id in the file.
 *
 * Anything absent is NONE. That is the whole of the forward-compatibility policy here: a board
 * on an architecture this build has never heard of is one we know nothing about, and the honest
 * row says so rather than offering it the nearest path.
 */
static const struct {
    const char *architecture;
    enum mesh_firmware_path path;
} k_architectures[] = {
    {"nrf52840", MESH_FIRMWARE_PATH_USB},
    {"rp2040", MESH_FIRMWARE_PATH_USB},
    {"rp2350", MESH_FIRMWARE_PATH_USB},
    {"esp32", MESH_FIRMWARE_PATH_BLE},
    {"esp32-s3", MESH_FIRMWARE_PATH_BLE},
    /* Named rather than left to fall through, because these are the ones somebody will come
       back to: their loader partition holds the pre-unified `bleota-c3.bin`, and current
       firmware refuses to boot into it. Not "we cannot speak that protocol" - the radio will
       not go there. */
    {"esp32-c3", MESH_FIRMWARE_PATH_NONE},
    {"esp32-c6", MESH_FIRMWARE_PATH_NONE},
    /* A Linux process pretending to be a radio. It updates the way any program does. */
    {"portduino", MESH_FIRMWARE_PATH_NONE},
};

enum mesh_firmware_path mesh_firmware_path_for_architecture(const char *architecture) {
    if (architecture == NULL || architecture[0] == '\0') {
        return MESH_FIRMWARE_PATH_NONE;
    }
    for (size_t i = 0; i < sizeof k_architectures / sizeof k_architectures[0]; ++i) {
        if (strcmp(k_architectures[i].architecture, architecture) == 0) {
            return k_architectures[i].path;
        }
    }
    return MESH_FIRMWARE_PATH_NONE;
}

/* ---- deviceHardware ---------------------------------------------------------------------- */

/*
 * Reads one board object into `board`, leaving the cursor after it.
 *
 * Every field is optional as far as this is concerned: the document has grown keys twice since
 * it was first published (`supportLevel`, `hasInkHud`) and it will grow more, so an unknown key
 * is skipped and a missing one leaves its field at whatever init gave it. The only thing that
 * makes a board *usable* is a target, and that is checked by the caller.
 */
static bool catalog_read_board(struct mesh_json *json, struct mesh_firmware_board *board) {
    memset(board, 0, sizeof *board);
    if (!mesh_json_enter_object(json)) {
        return false;
    }
    char key[32];
    while (mesh_json_next_key(json, key, sizeof key)) {
        bool read = false;
        if (strcmp(key, "hwModel") == 0) {
            uint64_t model = 0U;
            read = mesh_json_read_u64(json, &model);
            board->hw_model = (uint32_t)model;
        } else if (strcmp(key, "platformioTarget") == 0) {
            read = mesh_json_read_string(json, board->target, sizeof board->target);
        } else if (strcmp(key, "displayName") == 0) {
            read = mesh_json_read_string(json, board->name, sizeof board->name);
        } else if (strcmp(key, "architecture") == 0) {
            read = mesh_json_read_string(json, board->architecture, sizeof board->architecture);
        } else if (strcmp(key, "activelySupported") == 0) {
            read = mesh_json_read_bool(json, &board->actively_supported);
        } else if (strcmp(key, "requiresDfu") == 0) {
            read = mesh_json_read_bool(json, &board->requires_dfu);
        }
        /* A key we wanted whose value was the wrong type still has to be stepped over, or the
           walk desynchronises and every board after it reads as garbage. */
        if (!read && !mesh_json_skip_value(json)) {
            return false;
        }
    }
    board->path = mesh_firmware_path_for_architecture(board->architecture);
    return true;
}

bool mesh_firmware_boards_parse(const char *json_text, size_t len, uint32_t hw_model,
                                struct mesh_firmware_boards *out) {
    if (json_text == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);

    struct mesh_json json;
    mesh_json_init(&json, json_text, len);
    if (!mesh_json_enter_array(&json)) {
        return false;
    }
    while (mesh_json_next_element(&json)) {
        struct mesh_firmware_board board;
        if (!catalog_read_board(&json, &board)) {
            return false;
        }
        if (board.hw_model != hw_model || board.target[0] == '\0') {
            continue;
        }
        /* Counted before it is stored, so a tenth variant of one model is visible as an
           ambiguity even though there is nowhere to put it. */
        if (out->found < UINT8_MAX) {
            out->found++;
        }
        if (out->count < MESH_FIRMWARE_BOARDS_MAX) {
            out->entries[out->count++] = board;
        }
    }
    return true;
}

/* ---- the release index ------------------------------------------------------------------- */

static const char *catalog_channel_key(enum mesh_firmware_channel channel) {
    return channel == MESH_FIRMWARE_CHANNEL_ALPHA ? "alpha" : "stable";
}

bool mesh_firmware_release_parse(const char *json_text, size_t len,
                                 enum mesh_firmware_channel channel,
                                 struct mesh_firmware_release *out) {
    if (json_text == NULL || out == NULL || channel >= MESH_FIRMWARE_CHANNEL_COUNT) {
        return false;
    }
    memset(out, 0, sizeof *out);

    struct mesh_json json;
    mesh_json_init(&json, json_text, len);
    if (!mesh_json_object_find(&json, "releases")) {
        return false;
    }
    if (!mesh_json_object_find(&json, catalog_channel_key(channel))) {
        return false;
    }
    if (!mesh_json_enter_array(&json)) {
        return false;
    }
    if (!mesh_json_next_element(&json)) {
        return false; /* the channel exists and is empty */
    }

    /* The newest, which is the first: the index is ordered by upstream and these versions end
       in a build hash, so re-sorting them here would be sorting on something unordered. */
    if (!mesh_json_enter_object(&json)) {
        return false;
    }
    char key[32];
    char tag[MESH_FIRMWARE_VERSION_MAX + 2U];
    tag[0] = '\0';
    while (mesh_json_next_key(&json, key, sizeof key)) {
        bool read = false;
        if (strcmp(key, "id") == 0) {
            read = mesh_json_read_string(&json, tag, sizeof tag);
        } else if (strcmp(key, "zip_url") == 0) {
            read = mesh_json_read_string(&json, out->manifest_url, sizeof out->manifest_url);
        }
        if (!read && !mesh_json_skip_value(&json)) {
            return false;
        }
    }
    /*
     * The leading 'v' comes off, and what is left has to be something.
     *
     * Tested *after* the strip rather than before it, which is not a nicety: a JSON string may
     * carry an embedded NUL - `\u0000`, or a raw byte - and one right after the 'v' leaves a tag
     * that is non-empty as far as the document is concerned and empty as far as C is. `make
     * fuzz` found exactly that, and what it produced was a release the client would have
     * offered as an update to a version with no name.
     */
    const char *const version = tag[0] == 'v' ? tag + 1 : tag;
    if (version[0] == '\0') {
        return false;
    }
    mesh_str_copy(out->version, sizeof out->version, version);
    /*
     * The index calls it `zip_url` and for a current release it is a `.json` - the per-release
     * manifest. Older entries really do point at a per-platform zip, and one of those is not
     * something phase 2 can range-read a single board's image out of, so it is dropped rather
     * than carried as a URL that would fail later with a stranger error.
     */
    const size_t url_len = strlen(out->manifest_url);
    if (url_len < 5U || strcmp(out->manifest_url + url_len - 5U, ".json") != 0) {
        out->manifest_url[0] = '\0';
    }
    return true;
}

/* ---- the release's own manifest ----------------------------------------------------------- */

bool mesh_firmware_platform_parse(const char *json_text, size_t len, const char *target, char *out,
                                  size_t out_len) {
    if (json_text == NULL || target == NULL || out == NULL || out_len == 0U || target[0] == '\0') {
        return false;
    }
    out[0] = '\0';

    struct mesh_json json;
    mesh_json_init(&json, json_text, len);
    if (!mesh_json_object_find(&json, "targets")) {
        return false;
    }
    if (!mesh_json_enter_array(&json)) {
        return false;
    }
    while (mesh_json_next_element(&json)) {
        if (!mesh_json_enter_object(&json)) {
            return false;
        }
        char key[32];
        char board[MESH_FIRMWARE_TARGET_MAX];
        char platform[MESH_FIRMWARE_ARCH_MAX];
        board[0] = '\0';
        platform[0] = '\0';
        while (mesh_json_next_key(&json, key, sizeof key)) {
            bool read = false;
            if (strcmp(key, "board") == 0) {
                read = mesh_json_read_string(&json, board, sizeof board);
            } else if (strcmp(key, "platform") == 0) {
                read = mesh_json_read_string(&json, platform, sizeof platform);
            }
            if (!read && !mesh_json_skip_value(&json)) {
                return false;
            }
        }
        if (strcmp(board, target) == 0 && platform[0] != '\0') {
            /*
             * The walk stops here rather than running to the end of the array. That is not an
             * optimisation: this document lists a board once, and a reader that kept going
             * would let a later duplicate - the shape a bad merge produces - overwrite the
             * answer with whichever copy was last.
             */
            mesh_str_copy(out, out_len, platform);
            return true;
        }
    }
    return false;
}

/* ---- versions ---------------------------------------------------------------------------- */

/* Reads the leading run of digits and steps `cursor` past it and any single separator after.
   A component that is not a number reads as 0, which is what makes an empty string sort low. */
static uint32_t catalog_version_component(const char **cursor) {
    const char *at = *cursor;
    uint32_t value = 0U;
    while (*at >= '0' && *at <= '9') {
        /* A version component wider than this is not a version; clamping keeps the comparison
           total rather than wrapping it into a smaller number that would sort wrong. */
        if (value < 100000U) {
            value = value * 10U + (uint32_t)(*at - '0');
        }
        at++;
    }
    if (*at == '.') {
        at++;
    }
    *cursor = at;
    return value;
}

int mesh_firmware_version_compare(const char *left, const char *right) {
    const char *a = left != NULL ? left : "";
    const char *b = right != NULL ? right : "";
    if (*a == 'v') {
        a++;
    }
    if (*b == 'v') {
        b++;
    }
    for (int component = 0; component < 3; ++component) {
        const uint32_t one = catalog_version_component(&a);
        const uint32_t two = catalog_version_component(&b);
        if (one != two) {
            return one < two ? -1 : 1;
        }
    }
    /* Whatever is left is the build hash, and a hash has no order. Equal numbers are one
       release as far as this client is concerned. */
    return 0;
}

/* ---- the board's own manifest ------------------------------------------------------------ */

/* Reads one entry of the manifest's `files` array. Same rule as a board: every key is
   optional, an unknown one is skipped, and a wanted key whose value is the wrong type still
   has to be stepped over or the walk desynchronises. */
static bool catalog_read_image(struct mesh_json *json, struct mesh_firmware_image *image) {
    memset(image, 0, sizeof *image);
    if (!mesh_json_enter_object(json)) {
        return false;
    }
    char key[32];
    while (mesh_json_next_key(json, key, sizeof key)) {
        bool read = false;
        if (strcmp(key, "name") == 0) {
            read = mesh_json_read_string(json, image->name, sizeof image->name);
        } else if (strcmp(key, "md5") == 0) {
            read = mesh_json_read_string(json, image->md5, sizeof image->md5);
        } else if (strcmp(key, "part_name") == 0) {
            read = mesh_json_read_string(json, image->part, sizeof image->part);
        } else if (strcmp(key, "bytes") == 0) {
            read = mesh_json_read_u64(json, &image->bytes);
        }
        if (!read && !mesh_json_skip_value(json)) {
            return false;
        }
    }
    return true;
}

static bool catalog_read_files(struct mesh_json *json, struct mesh_firmware_manifest *out) {
    if (!mesh_json_enter_array(json)) {
        return false;
    }
    while (mesh_json_next_element(json)) {
        struct mesh_firmware_image image;
        if (!catalog_read_image(json, &image)) {
            return false;
        }
        /* A file with no name is not a file we could ask the zip for. */
        if (image.name[0] == '\0') {
            continue;
        }
        if (out->found < UINT8_MAX) {
            out->found++;
        }
        if (out->count < MESH_FIRMWARE_FILES_MAX) {
            out->files[out->count++] = image;
        }
    }
    return true;
}

bool mesh_firmware_manifest_parse(const char *json_text, size_t len,
                                  struct mesh_firmware_manifest *out) {
    if (json_text == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);

    struct mesh_json json;
    mesh_json_init(&json, json_text, len);
    if (!mesh_json_enter_object(&json)) {
        return false;
    }
    char key[32];
    while (mesh_json_next_key(&json, key, sizeof key)) {
        bool read = false;
        if (strcmp(key, "version") == 0) {
            read = mesh_json_read_string(&json, out->version, sizeof out->version);
        } else if (strcmp(key, "platformioTarget") == 0) {
            read = mesh_json_read_string(&json, out->target, sizeof out->target);
        } else if (strcmp(key, "mcu") == 0) {
            read = mesh_json_read_string(&json, out->mcu, sizeof out->mcu);
        } else if (strcmp(key, "architecture") == 0) {
            read = mesh_json_read_string(&json, out->architecture, sizeof out->architecture);
        } else if (strcmp(key, "hwModel") == 0) {
            uint64_t model = 0U;
            read = mesh_json_read_u64(&json, &model);
            out->hw_model = (uint32_t)model;
        } else if (strcmp(key, "requiresDfu") == 0) {
            read = mesh_json_read_bool(&json, &out->requires_dfu);
        } else if (strcmp(key, "files") == 0) {
            /*
             * Fatal rather than skipped, unlike every other key here. A failed scalar read
             * leaves the cursor on the value it refused, so skipping it puts the walk back in
             * step; a failed *array* read has already consumed part of the array, and there is
             * nothing to skip that would land anywhere meaningful. Reading on from there would
             * report a manifest assembled out of the wreckage.
             */
            if (!catalog_read_files(&json, out)) {
                return false;
            }
            read = true;
        }
        if (!read && !mesh_json_skip_value(&json)) {
            return false;
        }
    }
    return true;
}

bool mesh_firmware_manifest_describes(const struct mesh_firmware_manifest *manifest,
                                      const char *target, const char *version) {
    if (manifest == NULL || target == NULL || version == NULL || target[0] == '\0' ||
        version[0] == '\0') {
        return false;
    }
    return strcmp(manifest->target, target) == 0 && strcmp(manifest->version, version) == 0;
}

/* True when `name` ends in `suffix`. The UF2 selector, and the reason it is a suffix test
   rather than a `part_name` one is that an nRF52 manifest carries no `part_name` at all. */
static bool catalog_name_ends_with(const char *name, const char *suffix) {
    const size_t name_len = strlen(name);
    const size_t suffix_len = strlen(suffix);
    return name_len >= suffix_len && strcmp(name + name_len - suffix_len, suffix) == 0;
}

const struct mesh_firmware_image *
mesh_firmware_manifest_image(const struct mesh_firmware_manifest *manifest,
                             enum mesh_firmware_path path) {
    if (manifest == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < manifest->count; ++i) {
        const struct mesh_firmware_image *const file = &manifest->files[i];
        if (path == MESH_FIRMWARE_PATH_USB) {
            /* The bootloader takes a UF2 and nothing else, and an nRF52 manifest publishes
               exactly one. The `-ota.zip` beside it is the Nordic DFU package, which is a
               different protocol over a different bus and is not this path's file. */
            if (catalog_name_ends_with(file->name, ".uf2")) {
                return file;
            }
        } else if (path == MESH_FIRMWARE_PATH_BLE) {
            if (strcmp(file->part, "app0") == 0) {
                return file;
            }
        }
    }
    return NULL;
}
