/*
 * What the client can explain about where the user is standing.
 *
 * One job: read the nav, and answer with the paragraphs that belong to the screen behind it -
 * as catalog ids, never as text. The notes themselves live on the things they describe (a
 * section's beside its icon in settings.c, a field's in its own k_fields row), because what a
 * setting does is a property of the setting. This file only assembles.
 *
 * See docs/help.md.
 */

#include "mesh/ui/help.h"

#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <string.h>

/* Whether this nav has a settings section open - the only thing phase 1 can explain. The
   two list-shaped sections (Modules, and Channels before a slot is picked) count: they have a
   note of their own, and their rows are sections rather than fields, so a topic there is the
   overview and nothing else. */
static bool help_section_open(const struct mesh_ui_nav *nav,
                              enum mesh_ui_settings_section *out) {
    if (nav == NULL || nav->screen != MESH_UI_SCREEN_SETTINGS) {
        return false;
    }
    if (nav->settings_section == MESH_UI_SETTINGS_NO_SECTION) {
        return false;
    }
    *out = (enum mesh_ui_settings_section)nav->settings_section;
    return *out < MESH_UI_SETTINGS_SECTION_COUNT;
}

/*
 * The open section's rows, with no pending edits applied.
 *
 * Deliberately without them: help says what a setting *is*, never what it is set to, so an edit
 * waiting to be saved changes nothing about which paragraphs this screen carries. Passing the
 * edits would also make the help screen's contents depend on how far through an edit the user
 * is, which is a way for the row a press was aimed at to move out from under it.
 */
static uint32_t help_section_items(const struct mesh_ui_settings *settings,
                                   const struct mesh_ui_handshake_state *handshake,
                                   const struct mesh_ui_nav *nav,
                                   enum mesh_ui_settings_section section,
                                   struct mesh_ui_settings_item *items) {
    return mesh_ui_settings_items(settings, handshake, NULL, 0U, section, nav->settings_channel,
                                  items, MESH_UI_SETTINGS_ITEMS_MAX);
}

/* The note for one row, or MESH_STR_NONE. A row that is not a field - a heading, a read-only
   fact, an action - has no field to ask about and so has no note; the section's own paragraph
   is what covers those. */
static enum mesh_str_id help_item_note(const struct mesh_ui_settings_item *item) {
    if (item->field == MESH_UI_FIELD_NONE) {
        return MESH_STR_NONE;
    }
    return mesh_ui_settings_field_note(item->field);
}

bool mesh_ui_help_topic(const struct mesh_ui_settings *settings,
                        const struct mesh_ui_handshake_state *handshake,
                        const struct mesh_ui_nav *nav, struct mesh_ui_help_topic *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);

    enum mesh_ui_settings_section section;
    if (settings == NULL || !help_section_open(nav, &section)) {
        return false;
    }
    const enum mesh_str_id overview = mesh_ui_settings_section_note(section);
    if (overview == MESH_STR_NONE) {
        /* Every section is supposed to have one, and a test says so - but a section that
           somehow does not is a screen with nothing to say, and offering the press for it would
           be worse than not offering it. */
        return false;
    }

    out->title = MESH_STR_HELP_TITLE;
    out->entries[out->count].label = MESH_STR_NONE;
    out->entries[out->count].body = overview;
    out->count++;

    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    const uint32_t rows = help_section_items(settings, handshake, nav, section, items);
    for (uint32_t i = 0U; i < rows && out->count < MESH_UI_HELP_ENTRIES_MAX; ++i) {
        const enum mesh_str_id note = help_item_note(&items[i]);
        if (note == MESH_STR_NONE) {
            continue;
        }
        /* The row's own name, as an id rather than copied off the item: a topic is ids the
           whole way down, so a test can read one without a locale in force and a backend
           cannot end up holding English. */
        out->entries[out->count].label = mesh_ui_settings_field_label_id(items[i].field);
        out->entries[out->count].body = note;
        out->count++;
    }
    return true;
}

uint32_t mesh_ui_help_entry_for_row(const struct mesh_ui_settings *settings,
                                    const struct mesh_ui_handshake_state *handshake,
                                    const struct mesh_ui_nav *nav, uint32_t row) {
    enum mesh_ui_settings_section section;
    if (settings == NULL || !help_section_open(nav, &section)) {
        return 0U;
    }

    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    const uint32_t rows = help_section_items(settings, handshake, nav, section, items);
    if (row >= rows) {
        return 0U;
    }

    /* How many paragraphs stand above this row, plus the one the section opens with. */
    uint32_t entry = 1U;
    for (uint32_t i = 0U; i < row; ++i) {
        if (help_item_note(&items[i]) != MESH_STR_NONE) {
            entry++;
        }
    }
    /*
     * A row with no paragraph of its own lands on the nearest one above it instead of at the
     * top. That is the useful answer rather than merely the safe one: the paragraphs above a
     * row in a section are the ones about the setting it sits with, and falling back to the
     * overview every time would make the screen open at the top for two rows out of three.
     *
     * No clamp on the way out, and that is MESH_UI_HELP_ENTRIES_MAX's doing rather than an
     * omission: the entry count runs to one per row plus the overview, which is exactly what a
     * topic holds, so this cannot name an entry the topic does not have.
     */
    if (help_item_note(&items[row]) == MESH_STR_NONE) {
        entry--;
    }
    return entry;
}
