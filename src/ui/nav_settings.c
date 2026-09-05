#define _POSIX_C_SOURCE 200809L

/*
 * The settings editor: pending edits and the confirm sheets that gate them.
 *
 * Edits accumulate in the nav struct rather than going to the radio a field at a time, because
 * the firmware replaces a config section whole - so a section is edited locally and saved in one
 * write (built over in src/core/app_settings.c). An edit that puts the radio's own value back is
 * dropped rather than recorded, which is what keeps "toggle it twice" from queueing a write.
 */

#include "nav_internal.h"

#include "mesh/ui/settings.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <string.h>

/* ---- settings edits ----------------------------------------------------------------------- */

const struct mesh_ui_handshake_state *mesh_ui_nav_handshake(const struct mesh_ui_store *store) {
    return store->handshake_valid ? &store->handshake : NULL;
}

/* The row under the cursor in the open section, with (or without) the pending edits. */
bool mesh_ui_nav_settings_current(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                  bool with_edits, struct mesh_ui_settings_item *out) {
    if (nav->screen != MESH_UI_SCREEN_SETTINGS ||
        nav->settings_section == MESH_UI_SETTINGS_NO_SECTION) {
        return false;
    }
    return mesh_ui_settings_item(&store->settings, mesh_ui_nav_handshake(store),
                                 with_edits ? nav->settings_edits : NULL,
                                 with_edits ? nav->settings_edit_count : 0U,
                                 (enum mesh_ui_settings_section)nav->settings_section,
                                 nav->settings_channel, nav->cursor[MESH_UI_SCREEN_SETTINGS], out);
}

static void mesh_ui_nav_edit_remove(struct mesh_ui_nav *nav, enum mesh_ui_setting_field field) {
    for (uint8_t i = 0; i < nav->settings_edit_count; ++i) {
        if (nav->settings_edits[i].field != (uint8_t)field) {
            continue;
        }
        for (uint8_t j = i; j + 1U < nav->settings_edit_count; ++j) {
            nav->settings_edits[j] = nav->settings_edits[j + 1U];
        }
        nav->settings_edit_count--;
        memset(&nav->settings_edits[nav->settings_edit_count], 0, sizeof nav->settings_edits[0]);
        return;
    }
}

/* Records an edit for the row under the cursor; an edit that puts the radio's own value back
   is dropped instead, so toggling something twice leaves the section clean. */
static bool mesh_ui_nav_edit_set(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                 enum mesh_ui_setting_field field, uint32_t number,
                                 const char *text) {
    struct mesh_ui_settings_item base;
    if (!mesh_ui_nav_settings_current(nav, store, false, &base) || base.field != field) {
        return false;
    }
    bool same;
    if (base.kind == MESH_UI_SETTING_TEXT) {
        same = (text != NULL && strcmp(base.text, text) == 0);
    } else if (base.kind == MESH_UI_SETTING_KEY) {
        /* Keeping the key, or typing the very key the radio has, is no edit. */
        same = (number == MESH_UI_PSK_KEEP);
        if (number == MESH_UI_PSK_TYPED && text != NULL) {
            uint8_t current[MESH_UI_PSK_MAX];
            uint8_t typed[MESH_UI_PSK_MAX];
            size_t current_len = 0U;
            size_t typed_len = 0U;
            same = mesh_ui_settings_key_parse(base.text, current, sizeof current, &current_len) &&
                   mesh_ui_settings_key_parse(text, typed, sizeof typed, &typed_len) &&
                   current_len == typed_len && memcmp(current, typed, typed_len) == 0;
        }
    } else {
        same = (base.number == number);
    }
    if (same) {
        mesh_ui_nav_edit_remove(nav, field);
        return true;
    }
    struct mesh_ui_setting_edit *slot = NULL;
    for (uint8_t i = 0; i < nav->settings_edit_count; ++i) {
        if (nav->settings_edits[i].field == (uint8_t)field) {
            slot = &nav->settings_edits[i];
            break;
        }
    }
    if (slot == NULL) {
        if (nav->settings_edit_count >= MESH_UI_SETTINGS_EDITS_MAX) {
            return false;
        }
        slot = &nav->settings_edits[nav->settings_edit_count++];
    }
    memset(slot, 0, sizeof *slot);
    slot->field = (uint8_t)field;
    slot->number = number;
    if (text != NULL) {
        snprintf(slot->text, sizeof slot->text, "%s", text);
    }
    return true;
}

static void mesh_ui_nav_edits_clear(struct mesh_ui_nav *nav) {
    memset(nav->settings_edits, 0, sizeof nav->settings_edits);
    nav->settings_edit_count = 0U;
    nav->settings_discard_armed = false;
}

/* Opens the keyboard on a field's text. The Compose draft is parked until done or cancel. */
static void mesh_ui_nav_open_field_keyboard(struct mesh_ui_nav *nav,
                                            const struct mesh_ui_settings_item *item) {
    snprintf(nav->draft_saved, sizeof nav->draft_saved, "%s", nav->draft);
    snprintf(nav->draft, sizeof nav->draft, "%s", item->text);
    nav->keyboard_field = (uint8_t)item->field;
    nav->keyboard_open = true;
    nav->kb_row = 0U;
    nav->kb_col = 0U;
    nav->kb_layer = MESH_UI_KB_LOWER;
}

/* A, Left or Right on a row of an open section. Toggles flip, enums cycle, numbers step
   through their presets, text opens the keyboard. Read-only rows ignore the press. */
bool mesh_ui_nav_settings_edit_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   enum mesh_ui_key key) {
    struct mesh_ui_settings_item item;
    if (!mesh_ui_nav_settings_current(nav, store, true, &item) ||
        item.field == MESH_UI_FIELD_NONE) {
        return false;
    }
    const enum mesh_ui_setting_field field = item.field;
    const int delta = (key == MESH_UI_KEY_LEFT) ? -1 : +1;
    switch (item.kind) {
    case MESH_UI_SETTING_TOGGLE:
        return mesh_ui_nav_edit_set(nav, store, field, item.number != 0U ? 0U : 1U, NULL);
    case MESH_UI_SETTING_ENUM: {
        const uint32_t count = mesh_ui_settings_enum_count(field);
        if (count == 0U) {
            return false;
        }
        const uint32_t next =
            (item.number + count + (uint32_t)(delta < 0 ? count - 1U : 1U)) % count;
        return mesh_ui_nav_edit_set(nav, store, field, next, NULL);
    }
    case MESH_UI_SETTING_NUMBER: {
        const uint32_t next = mesh_ui_settings_number_step(field, item.number, delta);
        if (next == item.number) {
            return false;
        }
        return mesh_ui_nav_edit_set(nav, store, field, next, NULL);
    }
    case MESH_UI_SETTING_KEY:
        if (key == MESH_UI_KEY_A) {
            mesh_ui_nav_open_field_keyboard(nav, &item); /* the key as hex */
            return true;
        }
        {
            /* Left/Right walk the choices the field offers; a typed key counts as "keep". */
            const uint32_t allowed = mesh_ui_settings_key_choices(field);
            uint32_t choice = item.number >= MESH_UI_PSK_TYPED ? 0U : item.number;
            for (unsigned step = 0; step < (unsigned)MESH_UI_PSK_TYPED; ++step) {
                choice = (choice + (uint32_t)MESH_UI_PSK_TYPED +
                          (uint32_t)(delta < 0 ? (unsigned)MESH_UI_PSK_TYPED - 1U : 1U)) %
                         (uint32_t)MESH_UI_PSK_TYPED;
                if ((allowed & MESH_UI_PSK_CHOICE_BIT(choice)) != 0U) {
                    break;
                }
            }
            return mesh_ui_nav_edit_set(nav, store, field, choice, NULL);
        }
    case MESH_UI_SETTING_TEXT:
        if (key != MESH_UI_KEY_A) {
            return false;
        }
        mesh_ui_nav_open_field_keyboard(nav, &item);
        return true;
    default:
        return false;
    }
}

/* Done on the keyboard while it edits a setting: the draft becomes the pending edit. */
bool mesh_ui_nav_settings_commit_text(struct mesh_ui_nav *nav, const struct mesh_ui_store *store) {
    const enum mesh_ui_setting_field field = (enum mesh_ui_setting_field)nav->keyboard_field;
    char text[MESH_UI_SETTING_TEXT_MAX];
    mesh_str_copy(text, sizeof text, nav->draft);
    size_t cap = mesh_ui_settings_text_max(field);
    if (cap >= sizeof text) {
        cap = sizeof text - 1U;
    }
    if (strlen(text) > cap) {
        /* Never cut a UTF-8 sequence in half. */
        while (cap > 0U && ((unsigned char)text[cap] & 0xC0U) == 0x80U) {
            cap--;
        }
        text[cap] = '\0';
    }
    if (mesh_ui_settings_field_kind(field) == MESH_UI_SETTING_KEY) {
        uint8_t parsed[MESH_UI_PSK_MAX];
        size_t parsed_len = 0U;
        if (!mesh_ui_settings_key_parse(text, parsed, sizeof parsed, &parsed_len) ||
            !mesh_ui_settings_key_len_ok(field, parsed_len)) {
            return true; /* not a key this field takes: stay on the keyboard so it can be fixed */
        }
        mesh_ui_nav_edit_set(nav, store, field, MESH_UI_PSK_TYPED, text);
    } else {
        mesh_ui_nav_edit_set(nav, store, field, 0U, text);
    }
    mesh_ui_nav_keyboard_close(nav);
    return true;
}

/* Y with edits: emit the save, or ask first for sections that can cut us off. */
static void mesh_ui_nav_fill_save(const struct mesh_ui_nav *nav, struct mesh_ui_action *action) {
    if (action == NULL) {
        return;
    }
    action->type = MESH_UI_ACTION_SAVE_SETTINGS;
    action->section = nav->settings_section;
    action->channel = nav->settings_channel;
    action->edit_count = nav->settings_edit_count;
    memcpy(action->edits, nav->settings_edits, sizeof action->edits);
}

static void mesh_ui_nav_confirm_close(struct mesh_ui_nav *nav) {
    nav->confirm_open = false;
    nav->confirm_action = (uint8_t)MESH_UI_SETTINGS_ACTION_NONE;
}

/* A radio action carries the open section's pending edits for the same reason a save does:
   "Set fixed position" is a row that reads the three rows above it. */
void mesh_ui_nav_fill_radio_action(const struct mesh_ui_nav *nav,
                                   enum mesh_ui_settings_action which,
                                   struct mesh_ui_action *action) {
    if (action == NULL) {
        return;
    }
    action->type = MESH_UI_ACTION_RADIO_ACTION;
    action->section = nav->settings_section;
    action->number = (uint32_t)which;
    action->edit_count = nav->settings_edit_count;
    memcpy(action->edits, nav->settings_edits, sizeof action->edits);
}

bool mesh_ui_nav_confirm_key(struct mesh_ui_nav *nav, enum mesh_ui_key key,
                             struct mesh_ui_action *action) {
    switch (key) {
    case MESH_UI_KEY_UP:
    case MESH_UI_KEY_DOWN:
    case MESH_UI_KEY_LEFT:
    case MESH_UI_KEY_RIGHT:
        nav->confirm_cursor = nav->confirm_cursor == 0U ? 1U : 0U;
        return true;
    case MESH_UI_KEY_A:
    case MESH_UI_KEY_START:
        if (nav->confirm_cursor == 0U) {
            /* Two things stand behind this overlay: a section save, and a radio action that
               keeps no state and so has no edits to carry. */
            if (nav->confirm_action != (uint8_t)MESH_UI_SETTINGS_ACTION_NONE) {
                mesh_ui_nav_fill_radio_action(
                    nav, (enum mesh_ui_settings_action)nav->confirm_action, action);
            } else {
                mesh_ui_nav_fill_save(nav, action);
            }
        }
        mesh_ui_nav_confirm_close(nav);
        return true;
    case MESH_UI_KEY_B:
        mesh_ui_nav_confirm_close(nav);
        return true;
    default:
        return false;
    }
}

bool mesh_ui_nav_settings_back(struct mesh_ui_nav *nav) {
    if (nav->settings_section == MESH_UI_SETTINGS_NO_SECTION) {
        return false;
    }
    mesh_ui_nav_edits_clear(nav);
    if (nav->settings_channel != MESH_UI_SETTINGS_NO_CHANNEL) {
        nav->settings_channel = MESH_UI_SETTINGS_NO_CHANNEL;
        nav->cursor[MESH_UI_SCREEN_SETTINGS] = nav->settings_channel_list_cursor;
        return true;
    }
    /* A module was opened from the Modules list, so B goes back there rather than all the way
       out; the second B leaves Modules the ordinary way. */
    if (nav->settings_parent == MESH_UI_SETTINGS_MODULES) {
        nav->settings_parent = MESH_UI_SETTINGS_NO_SECTION;
        nav->settings_section = MESH_UI_SETTINGS_MODULES;
        nav->cursor[MESH_UI_SCREEN_SETTINGS] = nav->settings_module_list_cursor;
        return true;
    }
    nav->settings_section = MESH_UI_SETTINGS_NO_SECTION;
    nav->cursor[MESH_UI_SCREEN_SETTINGS] = nav->settings_list_cursor;
    return true;
}

/* Keys that mean something different while a Settings section is open: Left/Right edit the
   row instead of switching tabs (L1/R1 still do), Y saves, B asks before discarding edits.
   Returns false to let the ordinary handling run. */
bool mesh_ui_nav_settings_section_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                      enum mesh_ui_key key, struct mesh_ui_action *action,
                                      bool *handled) {
    *handled = true;
    switch (key) {
    case MESH_UI_KEY_LEFT:
    case MESH_UI_KEY_RIGHT:
        return mesh_ui_nav_settings_edit_key(nav, store, key);
    case MESH_UI_KEY_Y:
        if (nav->settings_edit_count == 0U) {
            return false;
        }
        if (mesh_ui_settings_section_needs_confirm(
                (enum mesh_ui_settings_section)nav->settings_section)) {
            nav->confirm_open = true;
            nav->confirm_cursor = 1U; /* Cancel, so a repeated press changes nothing */
            nav->confirm_action = (uint8_t)MESH_UI_SETTINGS_ACTION_NONE;
            return true;
        }
        mesh_ui_nav_fill_save(nav, action);
        return false; /* the app clears the edits once the write is queued */
    case MESH_UI_KEY_B:
        if (nav->settings_edit_count > 0U && !nav->settings_discard_armed) {
            nav->settings_discard_armed = true; /* the footer now says "B again to discard" */
            return true;
        }
        return mesh_ui_nav_settings_back(nav);
    default:
        *handled = false;
        return false;
    }
}
