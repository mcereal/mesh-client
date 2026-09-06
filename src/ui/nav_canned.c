#define _POSIX_C_SOURCE 200809L

/*
 * The canned-message list.
 *
 * The Brick has no keyboard worth typing a sentence on, so the fast path out of Compose is a
 * fixed list of ten phrases. It is file-scope state rather than part of struct mesh_ui_nav
 * because it is loaded once from disk and shared by every screen that can send.
 */

#include "nav_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/utils/array.h"
#include "mesh/utils/text.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Short, unambiguous, and what you actually want to say with no keyboard. Replaceable through
   mesh_ui_canned_load(), and translated: a canned reply is the one piece of text this client
   puts on the air, so it has to be in the language the sender speaks. */
static const enum mesh_str_id k_default_canned[] = {
    MESH_STR_CANNED_OK,        MESH_STR_CANNED_YES,           MESH_STR_CANNED_NO,
    MESH_STR_CANNED_ON_MY_WAY, MESH_STR_CANNED_WHERE_ARE_YOU, MESH_STR_CANNED_IM_HERE,
    MESH_STR_CANNED_CALL_ME,   MESH_STR_CANNED_NEED_HELP,     MESH_STR_CANNED_HEADING_BACK,
    MESH_STR_CANNED_PING,
};

static char s_canned[MESH_UI_CANNED_MAX][MESH_UI_CANNED_TEXT_MAX];
static size_t s_canned_count;
static bool s_canned_loaded;

static void mesh_ui_canned_defaults(void) {
    s_canned_count = 0U;
    for (size_t i = 0; i < MESH_ARRAY_LEN(k_default_canned) && s_canned_count < MESH_UI_CANNED_MAX;
         ++i) {
        snprintf(s_canned[s_canned_count], sizeof s_canned[0], "%s", mesh_str(k_default_canned[i]));
        s_canned_count++;
    }
    s_canned_loaded = true;
}

void mesh_ui_canned_reset(void) { mesh_ui_canned_defaults(); }

size_t mesh_ui_canned_count(void) {
    if (!s_canned_loaded) {
        mesh_ui_canned_defaults();
    }
    return s_canned_count;
}

const char *mesh_ui_canned_text(size_t index) {
    if (!s_canned_loaded) {
        mesh_ui_canned_defaults();
    }
    if (index >= s_canned_count) {
        return "";
    }
    return s_canned[index];
}

int mesh_ui_canned_load(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -errno;
    }

    char staged[MESH_UI_CANNED_MAX][MESH_UI_CANNED_TEXT_MAX];
    size_t count = 0U;
    char line[256];
    while (count < MESH_UI_CANNED_MAX && fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        /* Control bytes would reach the radio and the framebuffer as-is; drop the line. */
        bool clean = true;
        for (const unsigned char *c = (const unsigned char *)line; *c != '\0'; ++c) {
            if (*c < 0x20U) {
                clean = false;
                break;
            }
        }
        if (!clean || line[0] == '\0' || line[0] == '#') {
            continue;
        }
        mesh_str_copy(staged[count], sizeof staged[0], line);
        count++;
    }
    fclose(file);

    if (count == 0U) {
        return -ENODATA;
    }

    memcpy(s_canned, staged, sizeof s_canned);
    s_canned_count = count;
    s_canned_loaded = true;
    return (int)count;
}
