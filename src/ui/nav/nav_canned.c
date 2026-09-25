#define _POSIX_C_SOURCE 200809L

/*
 * The canned-message list.
 *
 * The Brick has no keyboard worth typing a sentence on, so the fast path out of Compose is a
 * fixed list of ten phrases. It is file-scope state rather than part of struct mesh_ui_nav
 * because it is loaded once from disk and shared by every screen that can send.
 */

#include "inkwell/base/array.h"
#include "inkwell/base/record_file.h"
#include "inkwell/base/text.h"

#include "nav_internal.h"

#include "mesh/i18n/strings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Short, unambiguous, and what you actually want to say with no keyboard. Replaceable through
   mesh_ui_canned_load(), and translated: a canned reply is the one piece of text this client
   puts on the air, so it has to be in the language the sender speaks. */
static const inkcell_str_id k_default_canned[] = {
    MESH_STR_CANNED_OK,        MESH_STR_CANNED_YES,           MESH_STR_CANNED_NO,
    MESH_STR_CANNED_ON_MY_WAY, MESH_STR_CANNED_WHERE_ARE_YOU, MESH_STR_CANNED_IM_HERE,
    MESH_STR_CANNED_CALL_ME,   MESH_STR_CANNED_NEED_HELP,     MESH_STR_CANNED_HEADING_BACK,
    MESH_STR_CANNED_PING,
};

static char s_canned[MESH_UI_CANNED_MAX][MESH_UI_CANNED_TEXT_MAX];
static size_t s_canned_count;
static bool s_canned_loaded;
static bool s_canned_custom;

static void mesh_ui_canned_defaults(void) {
    s_canned_custom = false;
    s_canned_count = 0U;
    for (size_t i = 0;
         i < INKWELL_ARRAY_LEN(k_default_canned) && s_canned_count < MESH_UI_CANNED_MAX; ++i) {
        snprintf(s_canned[s_canned_count], sizeof s_canned[0], "%s",
                 inkcell_str(k_default_canned[i]));
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
    return s_canned_custom ? s_canned[index] : inkcell_str(k_default_canned[index]);
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
        inkwell_str_copy(staged[count], sizeof staged[0], line);
        count++;
    }
    fclose(file);

    if (count == 0U) {
        return -ENODATA;
    }

    s_canned_custom = true;
    memcpy(s_canned, staged, sizeof s_canned);
    s_canned_count = count;
    s_canned_loaded = true;
    return (int)count;
}

bool mesh_ui_canned_accepts(const char *text) {
    if (text == NULL || text[0] == '\0' || text[0] == '#') {
        return false;
    }
    const size_t len = strlen(text);
    if (len >= MESH_UI_CANNED_TEXT_MAX) {
        return false;
    }
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; ++c) {
        if (*c < 0x20U) {
            return false;
        }
    }
    const size_t count = mesh_ui_canned_count();
    if (count >= MESH_UI_CANNED_MAX) {
        return false;
    }
    for (size_t i = 0U; i < count; ++i) {
        if (strcmp(mesh_ui_canned_text(i), text) == 0) {
            return false;
        }
    }
    return true;
}

/* The list as it stands, one line each, then the line joining it. */
static void mesh_ui_canned_write(FILE *file, void *context) {
    const size_t count = mesh_ui_canned_count();
    for (size_t i = 0U; i < count; ++i) {
        fprintf(file, "%s\n", mesh_ui_canned_text(i));
    }
    fprintf(file, "%s\n", (const char *)context);
}

int mesh_ui_canned_add(const char *path, const char *text) {
    if (path == NULL || path[0] == '\0' || !mesh_ui_canned_accepts(text)) {
        return -EINVAL;
    }
    char temp[512];
    const int written =
        inkwell_record_replace(path, temp, sizeof temp, mesh_ui_canned_write, (void *)text, true);
    if (written < 0) {
        return written;
    }
    return mesh_ui_canned_load(path);
}
