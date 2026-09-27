#include "mesh/core/firmware_meshcore.h"

#include "inkwell/base/text.h"
#include "inkwell/codec/json.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

#define MESHCORE_TAG_PREFIX "refs/tags/"
#define MESHCORE_COMPANION "companion-v"
/* A pattern as the flasher writes one: "Heltec_t114_without_display_companion_radio_usb.*?\.zip"
   is 57. */
#define MESHCORE_PATTERN_MAX 128U

/*
 * The radios whose own name is not the flasher's, as DEVICE_INFO's manufacturer string to the
 * device it is in config.json. Everything else matches by name ignoring case ("Heltec V3" is
 * "Heltec v3"), so a row here is a real disagreement between the firmware and its flasher, read
 * off the board classes' getManufacturerName() and the flasher's device list side by side.
 * A board missing from both is the unknown-board refusal, which is honest.
 */
static const struct {
    const char *model;
    const char *device;
} k_aliases[] = {
    {"Elecrow ThinkNode-M1", "Elecrow ThinkNode M1"},
    {"GAT562 30S Mesh Kit", "GAT-IoT GAT562 30s"},
    {"GAT562 Mesh Tracker Pro", "GAT-IoT GAT562 Tracker"},
    {"Heltec E213", "Heltec Vision Master E213"},
    {"Heltec E290", "Heltec Vision Master E290"},
    {"Heltec Mesh Solar", "Heltec MeshSolar / MeshTower"},
    {"Heltec T096", "Heltec Mesh Node T096"},
    {"Heltec T1", "Heltec Mesh Node T1"},
    {"Heltec Tower V2", "Heltec MeshTower V2"},
    {"Heltec Tracker V2", "Heltec Wireless Tracker v2"},
    {"Heltec Wireless Paper", "Heltec Heltec Wireless Paper"},
    {"LILYGO T-LoRa V2.1-1.6", "LilyGo LoRa32 V2.1_1.6"},
    {"Nano G2 Ultra", "UnitEng Nano G2 Ultra"},
    {"ProMicro DIY", "ProMicro nrf52 (faketec)"},
    {"RAK 3112", "RAK WisBlock 3112"},
    {"RAK 3401", "RAK WisMesh 1W Booster (3401 + 13302)"},
    {"RAK 4631", "RAK WisBlock / WisMesh (RAK 4631)"},
    {"Seeed SenseCAP MeshTracker X1", "Seeed Studio SenseCAP MeshTracker X1"},
    {"Seeed SenseCap Solar", "Seeed Studio SenseCAP Solar Node P1"},
    {"Seeed Tracker T1000-E", "Seeed Studio SenseCAP T1000-E"},
    {"Seeed Xiao-nrf52", "Seeed Studio Xiao nRF52 WIO"},
    {"Station G2", "UnitEng Station G2"},
    {"Station G3 ESP32", "UnitEng/BQ Voyage Station G3"},
    {"Xiao C3", "Seeed Studio Xiao C3"},
    {"Xiao S3 WIO", "Seeed Studio Xiao S3 WIO"},
};

/*
 * One board under both firmwares: the flasher's device name, and the Meshtastic build of the
 * same hardware by its `platformioTarget`. Only pairs that are the same board and one build on
 * each side - a device Meshtastic splits by battery or radio (the MeshPocket, the Heltec V2)
 * or names by a different module (RAK's 3112 and 3312) is left out, and a board missing here
 * is simply not offered the switch.
 */
static const struct {
    const char *device;
    const char *target;
} k_twins[] = {
    {"Elecrow ThinkNode M1", "thinknode_m1"},
    {"Elecrow ThinkNode M2", "thinknode_m2"},
    {"Elecrow ThinkNode M3", "thinknode_m3"},
    {"Elecrow ThinkNode M5", "thinknode_m5"},
    {"Elecrow ThinkNode M6", "thinknode_m6"},
    {"Elecrow ThinkNode M7", "thinknode_m7"},
    {"Heltec Mesh Node T096", "heltec-mesh-node-t096"},
    {"Heltec Mesh Node T1", "heltec-mesh-node-t1"},
    {"Heltec T114", "heltec-mesh-node-t114"},
    {"Heltec Vision Master E213", "heltec-vision-master-e213"},
    {"Heltec Vision Master E290", "heltec-vision-master-e290"},
    {"Heltec WSL3", "heltec-wsl-v3"},
    {"Heltec Wireless Tracker v2", "heltec-wireless-tracker-v2"},
    {"Heltec v3", "heltec-v3"},
    {"Heltec v4", "heltec-v4"},
    {"LilyGo T-Beam 1W", "t-beam-1w"},
    {"LilyGo T-Beam Supreme (SX1262)", "tbeam-s3-core"},
    {"LilyGo T-Deck", "t-deck"},
    {"LilyGo T-Echo", "t-echo"},
    {"RAK WisBlock / WisMesh (RAK 4631)", "rak4631"},
    {"RAK WisMesh Tag", "rak_wismeshtag"},
    {"Seeed Studio SenseCAP T1000-E", "tracker-t1000-e"},
    {"Seeed Studio Xiao S3 WIO", "seeed-xiao-s3"},
    {"UnitEng Station G2", "station-g2"},
};

const char *mesh_firmware_meshcore_device_for_target(const char *target) {
    for (size_t i = 0; target != NULL && i < sizeof k_twins / sizeof k_twins[0]; ++i) {
        if (strcmp(target, k_twins[i].target) == 0) {
            return k_twins[i].device;
        }
    }
    return NULL;
}

const char *mesh_firmware_meshcore_target_for_device(const char *device) {
    for (size_t i = 0; device != NULL && i < sizeof k_twins / sizeof k_twins[0]; ++i) {
        if (strcmp(device, k_twins[i].device) == 0) {
            return k_twins[i].target;
        }
    }
    return NULL;
}

bool mesh_firmware_meshcore_names(const char *model, const char *device) {
    if (model == NULL || device == NULL || model[0] == '\0') {
        return false;
    }
    if (strcasecmp(model, device) == 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof k_aliases / sizeof k_aliases[0]; ++i) {
        if (strcasecmp(model, k_aliases[i].model) == 0 &&
            strcasecmp(device, k_aliases[i].device) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * The literal a pattern starts with - the build's name - into `out`. Stops at the first
 * character that means something to a regex, and refuses a pattern that has none, or whose
 * name was cut short by `out`, since a shortened name is a prefix of some other build's.
 */
static bool meshcore_pattern_prefix(const char *pattern, char *out, size_t out_len) {
    size_t len = strcspn(pattern, ".*?+[](){}|^$\\");
    if (len < 4U || len >= out_len) {
        return false;
    }
    memcpy(out, pattern, len);
    out[len] = '\0';
    return true;
}

/*
 * One `firmware` entry: its role, and the pattern for the file an install writes - the app
 * image (`flash-update`) of an ESP32 build, the `.uf2` (`download`) of an nRF52 one.
 */
static bool meshcore_read_build(struct inkwell_json *json, bool esp32, char *role, size_t role_len,
                                char *pattern, size_t pattern_len) {
    role[0] = '\0';
    pattern[0] = '\0';
    if (!inkwell_json_enter_object(json)) {
        return false;
    }
    char key[32];
    while (inkwell_json_next_key(json, key, sizeof key)) {
        bool read = false;
        if (strcmp(key, "role") == 0) {
            read = inkwell_json_read_string(json, role, role_len);
        } else if (strcmp(key, "github") == 0 && inkwell_json_enter_object(json)) {
            char inner[32];
            while (inkwell_json_next_key(json, inner, sizeof inner)) {
                bool took = false;
                if (strcmp(inner, "files") == 0 && inkwell_json_enter_object(json)) {
                    char file[32];
                    while (inkwell_json_next_key(json, file, sizeof file)) {
                        bool wanted = strcmp(file, esp32 ? "flash-update" : "download") == 0;
                        if (!(wanted && inkwell_json_read_string(json, pattern, pattern_len)) &&
                            !inkwell_json_skip_value(json)) {
                            return false;
                        }
                    }
                    took = true;
                }
                if (!took && !inkwell_json_skip_value(json)) {
                    return false;
                }
            }
            read = true;
        }
        if (!read && !inkwell_json_skip_value(json)) {
            return false;
        }
    }
    return true;
}

/* The builds of one device's `firmware` array, from a cursor on it, adding each of `role`. */
static bool meshcore_read_builds(struct inkwell_json *json, const char *name, const char *type,
                                 const char *role, struct mesh_firmware_boards *out) {
    if (!inkwell_json_enter_array(json)) {
        return false;
    }
    const bool esp32 = strcmp(type, "esp32") == 0;
    while (inkwell_json_next_element(json)) {
        char build_role[32];
        char pattern[MESHCORE_PATTERN_MAX];
        if (!meshcore_read_build(json, esp32, build_role, sizeof build_role, pattern,
                                 sizeof pattern)) {
            return false;
        }
        struct mesh_firmware_board board;
        memset(&board, 0, sizeof board);
        if (strcmp(build_role, role) != 0 ||
            !meshcore_pattern_prefix(pattern, board.target, sizeof board.target)) {
            continue;
        }
        inkwell_str_copy(board.name, sizeof board.name, name);
        inkwell_str_copy(board.architecture, sizeof board.architecture,
                         esp32                        ? "esp32"
                         : strcmp(type, "nrf52") == 0 ? "nrf52840"
                                                      : type);
        board.actively_supported = true;
        board.meshcore = true;
        /* Over USB either way: an ESP32 through its ROM, an nRF52 through the UF2 bootloader a
           1200-baud touch reaches. Anything else the flasher lists has no path from here. */
        board.path = esp32 || strcmp(board.architecture, "nrf52840") == 0 ? MESH_FIRMWARE_PATH_USB
                                                                          : MESH_FIRMWARE_PATH_NONE;
        if (out->found < UINT8_MAX) {
            out->found++;
        }
        if (out->count < MESH_FIRMWARE_BOARDS_MAX) {
            out->entries[out->count++] = board;
        }
    }
    return true;
}

/*
 * One device, adding each build of `role` to `out` when `model` names it - or, with `model`
 * NULL, whatever it is called.
 *
 * Whether it does is only known once `name` and `type` are, and a JSON object's keys come in
 * any order - so the `firmware` array is stepped over where it is met, with a copy of the
 * cursor kept on it, and read from that copy once the whole object has been.
 */
static bool meshcore_read_device(struct inkwell_json *json, const char *model, const char *role,
                                 struct mesh_firmware_boards *out) {
    if (!inkwell_json_enter_object(json)) {
        return false;
    }
    char name[MESH_FIRMWARE_BOARD_NAME_MAX] = "";
    char type[MESH_FIRMWARE_ARCH_MAX] = "";
    struct inkwell_json builds;
    bool have_builds = false;
    char key[32];
    while (inkwell_json_next_key(json, key, sizeof key)) {
        bool read = false;
        if (strcmp(key, "name") == 0) {
            read = inkwell_json_read_string(json, name, sizeof name);
        } else if (strcmp(key, "type") == 0) {
            read = inkwell_json_read_string(json, type, sizeof type);
        } else if (strcmp(key, "firmware") == 0) {
            builds = *json;
            have_builds = true;
        }
        if (!read && !inkwell_json_skip_value(json)) {
            return false;
        }
    }
    if (!have_builds || name[0] == '\0' || type[0] == '\0' ||
        (model != NULL && !mesh_firmware_meshcore_names(model, name))) {
        return true;
    }
    return meshcore_read_builds(&builds, name, type, role, out);
}

bool mesh_firmware_meshcore_boards_parse(const char *json_text, size_t len, const char *model,
                                         bool usb, struct mesh_firmware_boards *out) {
    if (json_text == NULL || model == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    struct inkwell_json json;
    inkwell_json_init(&json, json_text, len);
    if (!inkwell_json_object_find(&json, "device") || !inkwell_json_enter_array(&json)) {
        return false;
    }
    if (model[0] == '\0') {
        return true;
    }
    const char *const role = usb ? "companionUsb" : "companionBle";
    while (inkwell_json_next_element(&json)) {
        if (!meshcore_read_device(&json, model, role, out)) {
            return false;
        }
    }
    return true;
}

bool mesh_firmware_meshcore_usb_devices(const char *json_text, size_t len,
                                        struct mesh_firmware_choices *out) {
    if (json_text == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    struct inkwell_json json;
    inkwell_json_init(&json, json_text, len);
    if (!inkwell_json_object_find(&json, "device") || !inkwell_json_enter_array(&json)) {
        return false;
    }
    while (inkwell_json_next_element(&json)) {
        struct mesh_firmware_boards builds;
        memset(&builds, 0, sizeof builds);
        if (!meshcore_read_device(&json, NULL, "companionUsb", &builds)) {
            return false;
        }
        /* Two builds under one name is a choice the name cannot make, so it is not offered. */
        if (builds.found != 1U || strcmp(builds.entries[0].architecture, "esp32") != 0 ||
            out->count >= MESH_FIRMWARE_CHOICES_MAX) {
            continue;
        }
        inkwell_str_copy(out->names[out->count], sizeof out->names[out->count],
                         builds.entries[0].name);
        out->count++;
    }
    return true;
}

bool mesh_firmware_meshcore_latest_tag(const char *json_text, size_t len, char *tag, size_t tag_len,
                                       char *version, size_t version_len) {
    if (json_text == NULL || tag == NULL || version == NULL || tag_len == 0U || version_len == 0U) {
        return false;
    }
    tag[0] = '\0';
    version[0] = '\0';
    struct inkwell_json json;
    inkwell_json_init(&json, json_text, len);
    if (!inkwell_json_enter_array(&json)) {
        return false;
    }
    while (inkwell_json_next_element(&json)) {
        if (!inkwell_json_enter_object(&json)) {
            return false;
        }
        char ref[64] = "";
        char key[32];
        while (inkwell_json_next_key(&json, key, sizeof key)) {
            if (!(strcmp(key, "ref") == 0 && inkwell_json_read_string(&json, ref, sizeof ref)) &&
                !inkwell_json_skip_value(&json)) {
                return false;
            }
        }
        const size_t prefix = strlen(MESHCORE_TAG_PREFIX);
        if (strncmp(ref, MESHCORE_TAG_PREFIX MESHCORE_COMPANION,
                    prefix + strlen(MESHCORE_COMPANION)) != 0) {
            continue;
        }
        const char *const name = ref + prefix;
        const char *const number = name + strlen(MESHCORE_COMPANION);
        /* A tag with no number, or one cut short by `tag`, is not one to offer. */
        if (number[0] < '0' || number[0] > '9' || strlen(name) >= tag_len ||
            strlen(number) >= version_len) {
            continue;
        }
        if (tag[0] == '\0' || mesh_firmware_version_compare(number, version) > 0) {
            inkwell_str_copy(tag, tag_len, name);
            inkwell_str_copy(version, version_len, number);
        }
    }
    return tag[0] != '\0';
}

static bool meshcore_ends_with(const char *text, const char *suffix) {
    const size_t len = strlen(text);
    const size_t suffix_len = strlen(suffix);
    return len >= suffix_len && strcmp(text + len - suffix_len, suffix) == 0;
}

/* The one file an install of `board` writes, by name: with `wipe`, an ESP32's whole flash. */
static bool meshcore_asset_is(const char *name, const struct mesh_firmware_board *board,
                              bool wipe) {
    const size_t prefix = strlen(board->target);
    if (strncmp(name, board->target, prefix) != 0 || name[prefix] != '-') {
        return false;
    }
    if (mesh_firmware_architecture_uses_esp_rom(board->architecture)) {
        return wipe ? meshcore_ends_with(name, "-merged.bin")
                    : meshcore_ends_with(name, ".bin") && !meshcore_ends_with(name, "-merged.bin");
    }
    return meshcore_ends_with(name, ".uf2");
}

bool mesh_firmware_meshcore_asset_parse(const char *json_text, size_t len,
                                        const struct mesh_firmware_board *board,
                                        struct mesh_firmware_release *release) {
    if (json_text == NULL || board == NULL || release == NULL || board->target[0] == '\0') {
        return false;
    }
    release->image_name[0] = '\0';
    release->image_url[0] = '\0';
    release->image_bytes = 0U;
    struct inkwell_json json;
    inkwell_json_init(&json, json_text, len);
    if (!inkwell_json_object_find(&json, "assets") || !inkwell_json_enter_array(&json)) {
        return false;
    }
    unsigned matched = 0U;
    while (inkwell_json_next_element(&json)) {
        if (!inkwell_json_enter_object(&json)) {
            return false;
        }
        char name[MESH_FIRMWARE_FILE_NAME_MAX + 1U] = "";
        char url[MESH_FIRMWARE_URL_MAX + 1U] = "";
        uint64_t size = 0U;
        char key[32];
        while (inkwell_json_next_key(&json, key, sizeof key)) {
            bool read = false;
            if (strcmp(key, "name") == 0) {
                read = inkwell_json_read_string(&json, name, sizeof name);
            } else if (strcmp(key, "browser_download_url") == 0) {
                read = inkwell_json_read_string(&json, url, sizeof url);
            } else if (strcmp(key, "size") == 0) {
                read = inkwell_json_read_u64(&json, &size);
            }
            if (!read && !inkwell_json_skip_value(&json)) {
                return false;
            }
        }
        /* One byte of room past each field's own limit, so a name or URL that would not fit
           is seen as too long rather than truncated into a different one. */
        if (!meshcore_asset_is(name, board, release->wipe) ||
            strlen(name) >= MESH_FIRMWARE_FILE_NAME_MAX || strlen(url) >= MESH_FIRMWARE_URL_MAX ||
            strncmp(url, "https://", 8U) != 0 || size == 0U) {
            continue;
        }
        matched++;
        inkwell_str_copy(release->image_name, sizeof release->image_name, name);
        inkwell_str_copy(release->image_url, sizeof release->image_url, url);
        release->image_bytes = size;
    }
    if (matched != 1U) {
        release->image_name[0] = '\0';
        release->image_url[0] = '\0';
        release->image_bytes = 0U;
        return false;
    }
    return true;
}
