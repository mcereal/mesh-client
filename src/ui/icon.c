#include "mesh/ui/icon.h"

#include <string.h>

/* The glyph names, in enum order and from the same list the sprites were generated from. */
static const char *const k_names[MESH_UI_ICON_COUNT] = {
    "",
#define MESH_ICON_ENTRY(id, glyph) glyph,
#include "mesh/ui/icons.def"
#undef MESH_ICON_ENTRY
};

bool mesh_ui_icon_is_valid(enum mesh_ui_icon icon) {
    return icon > MESH_UI_ICON_NONE && icon < MESH_UI_ICON_COUNT;
}

const char *mesh_ui_icon_name(enum mesh_ui_icon icon) {
    return mesh_ui_icon_is_valid(icon) ? k_names[icon] : "";
}

void mesh_ui_icon_alpha(enum mesh_ui_icon icon,
                        uint8_t out[MESH_UI_ICON_SIZE * MESH_UI_ICON_SIZE]) {
    const size_t pixels = (size_t)MESH_UI_ICON_SIZE * MESH_UI_ICON_SIZE;
    if (out == NULL) {
        return;
    }
    memset(out, 0, pixels);
    if (!mesh_ui_icon_is_valid(icon)) {
        /* Including MESH_UI_ICON_NONE, whose sprite is blank anyway: an empty slot and a slot
           holding an id from a newer build both come out as nothing drawn. */
        return;
    }

    const struct mesh_ui_icon_table *table = &mesh_ui_icon_table;
    const uint32_t start = table->run_offsets[icon];
    const uint32_t end = table->run_offsets[icon + 1U];

    size_t written = 0;
    for (uint32_t run = start; run < end && written < pixels; ++run) {
        const uint8_t count = table->runs[run * 2U];
        const uint8_t alpha = table->runs[run * 2U + 1U];
        for (uint8_t i = 0; i < count && written < pixels; ++i) {
            out[written++] = alpha;
        }
    }
}
