#include "mesh/core/firmware_tern.h"

#include "inkwell/base/text.h"
#include "inkwell/base/version.h"
#include "inkwell/codec/json.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

int mesh_firmware_tern_version_compare(const char *left, const char *right) {
    return inkwell_version_compare(left != NULL ? left : "", right != NULL ? right : "");
}

/* What the apps hold a file name to: letters, digits, '.', '_' and '-', not starting with a
   dot - so it is a name in the feed's directory and never a path out of it. */
static bool tern_file_name_ok(const char *name) {
    if (name[0] == '\0' || name[0] == '.') {
        return false;
    }
    for (const char *c = name; *c != '\0'; ++c) {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                        (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* 64 hex digits, lowered into `out`. */
static bool tern_digest_read(const char *hex, char out[MESH_FIRMWARE_SHA256_HEX + 1U]) {
    if (strlen(hex) != MESH_FIRMWARE_SHA256_HEX) {
        return false;
    }
    for (size_t i = 0; i < MESH_FIRMWARE_SHA256_HEX; ++i) {
        char c = hex[i];
        if (c >= 'A' && c <= 'F') {
            c = (char)(c - 'A' + 'a');
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
        out[i] = c;
    }
    out[MESH_FIRMWARE_SHA256_HEX] = '\0';
    return true;
}

/* One image's entry, with one byte of room past each field's own limit, so a value that would
   not fit is seen as too long rather than truncated into a different one. */
struct tern_entry {
    char board[MESH_FIRMWARE_TARGET_MAX + 1U];
    char region[MESH_FIRMWARE_TERN_REGION_MAX + 1U];
    char file[MESH_FIRMWARE_FILE_NAME_MAX + 1U];
    char sha256[MESH_FIRMWARE_SHA256_HEX + 2U];
    uint64_t size;
};

static bool tern_entry_read(struct inkwell_json *json, struct tern_entry *entry) {
    memset(entry, 0, sizeof *entry);
    if (!inkwell_json_enter_object(json)) {
        return false;
    }
    char key[32];
    while (inkwell_json_next_key(json, key, sizeof key)) {
        bool read = false;
        if (strcmp(key, "board") == 0) {
            read = inkwell_json_read_string(json, entry->board, sizeof entry->board);
        } else if (strcmp(key, "region") == 0) {
            read = inkwell_json_read_string(json, entry->region, sizeof entry->region);
        } else if (strcmp(key, "file") == 0) {
            read = inkwell_json_read_string(json, entry->file, sizeof entry->file);
        } else if (strcmp(key, "sha256") == 0) {
            read = inkwell_json_read_string(json, entry->sha256, sizeof entry->sha256);
        } else if (strcmp(key, "size") == 0) {
            read = inkwell_json_read_u64(json, &entry->size);
        }
        if (!read && !inkwell_json_skip_value(json)) {
            return false;
        }
    }
    return true;
}

bool mesh_firmware_tern_parse(const char *json_text, size_t len, const char *board,
                              const char *region, const char *base_url,
                              struct mesh_firmware_boards *boards,
                              struct mesh_firmware_release *release) {
    if (json_text == NULL || boards == NULL || release == NULL) {
        return false;
    }
    memset(boards, 0, sizeof *boards);
    memset(release, 0, sizeof *release);
    const char *const want_board = board != NULL ? board : "";
    const char *const want_region = region != NULL ? region : "";
    const char *const base = base_url != NULL ? base_url : "";

    struct inkwell_json json;
    inkwell_json_init(&json, json_text, len);
    if (!inkwell_json_enter_object(&json)) {
        return false;
    }
    char version[MESH_FIRMWARE_VERSION_MAX + 1U] = "";
    bool have_images = false;
    struct tern_entry match;
    memset(&match, 0, sizeof match);
    char key[32];
    while (inkwell_json_next_key(&json, key, sizeof key)) {
        if (strcmp(key, "release") == 0) {
            if (!inkwell_json_read_string(&json, version, sizeof version)) {
                return false;
            }
            continue;
        }
        if (strcmp(key, "images") != 0) {
            if (!inkwell_json_skip_value(&json)) {
                return false;
            }
            continue;
        }
        if (!inkwell_json_enter_array(&json)) {
            return false;
        }
        have_images = true;
        while (inkwell_json_next_element(&json)) {
            struct tern_entry entry;
            if (!tern_entry_read(&json, &entry)) {
                return false;
            }
            char digest[MESH_FIRMWARE_SHA256_HEX + 1U];
            if (want_board[0] == '\0' || want_region[0] == '\0' ||
                strcasecmp(entry.board, want_board) != 0 ||
                strcasecmp(entry.region, want_region) != 0 ||
                strlen(entry.board) >= MESH_FIRMWARE_TARGET_MAX ||
                strlen(entry.region) >= MESH_FIRMWARE_TERN_REGION_MAX ||
                strlen(entry.file) >= MESH_FIRMWARE_FILE_NAME_MAX ||
                !tern_file_name_ok(entry.file) || entry.size == 0U ||
                entry.size > MESH_FIRMWARE_TERN_IMAGE_MAX ||
                !tern_digest_read(entry.sha256, digest)) {
                continue;
            }
            inkwell_str_copy(entry.sha256, sizeof entry.sha256, digest);
            if (boards->found < UINT8_MAX) {
                boards->found++;
            }
            match = entry;
        }
    }
    /* A release that is not SemVer is not one this client can order, so it is not one it can
       offer: unparseable sorts below everything, and below "" is nowhere. */
    if (!have_images || strlen(version) >= MESH_FIRMWARE_VERSION_MAX ||
        inkwell_version_compare(version, "") <= 0) {
        memset(boards, 0, sizeof *boards);
        return false;
    }
    inkwell_str_copy(release->version, sizeof release->version, version);
    if (boards->found != 1U) {
        return true;
    }
    char url[MESH_FIRMWARE_URL_MAX + 1U];
    const int written = snprintf(url, sizeof url, "%s%s", base, match.file);
    if (written < 0 || (size_t)written >= MESH_FIRMWARE_URL_MAX ||
        strncmp(url, "https://", 8U) != 0) {
        /* Nowhere to fetch it from: the version still stands, the image does not. */
        boards->found = 0U;
        return true;
    }
    struct mesh_firmware_board *const entry = &boards->entries[0];
    inkwell_str_copy(entry->target, sizeof entry->target, match.board);
    inkwell_str_copy(entry->name, sizeof entry->name, match.board);
    entry->actively_supported = true;
    entry->path = MESH_FIRMWARE_PATH_LINK;
    boards->count = 1U;
    inkwell_str_copy(release->image_name, sizeof release->image_name, match.file);
    inkwell_str_copy(release->image_url, sizeof release->image_url, url);
    release->image_bytes = match.size;
    inkwell_str_copy(release->image_sha256, sizeof release->image_sha256, match.sha256);
    return true;
}
