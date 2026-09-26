#define _POSIX_C_SOURCE 200809L

/*
 * The settings editor: pending edits and the confirm sheets that gate them.
 *
 * Edits accumulate in the nav struct rather than going to the radio a field at a time, because
 * the firmware replaces a config section whole - so a section is edited locally and saved in one
 * write (built over in src/app/app_settings.c). An edit that puts the radio's own value back is
 * dropped rather than recorded, which is what keeps "toggle it twice" from queueing a write.
 */

#include "inkwell/base/text.h"

#include "nav_internal.h"

#include "mesh/ui/focus.h"
#include "mesh/ui/settings.h"

#include <stdio.h>
#include <string.h>

/* ---- settings edits ----------------------------------------------------------------------- */

const struct mesh_ui_handshake_state *mesh_ui_nav_handshake(const struct mesh_ui_store *store) {
    return store->handshake_valid ? &store->handshake : NULL;
}

/*
 * Whether the pending edits belong to the section on the panel.
 *
 * They are the Settings tab's: a Radio tab page has no fields to edit, and a Reboot pressed there
 * carrying whatever was half-typed into Position on the other tab would be a write nobody made.
 */
static bool mesh_ui_nav_edits_apply(const struct mesh_ui_nav *nav, bool with_edits) {
    return with_edits && nav->screen == MESH_UI_SCREEN_SETTINGS;
}

/* The row under the cursor in the open section, with (or without) the pending edits. */
bool mesh_ui_nav_settings_current(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                  bool with_edits, struct mesh_ui_settings_item *out) {
    const uint8_t section = mesh_ui_nav_open_section(nav);
    if (section == MESH_UI_SETTINGS_NO_SECTION) {
        return false;
    }
    const bool edits = mesh_ui_nav_edits_apply(nav, with_edits);
    return mesh_ui_settings_item(
        &store->settings, mesh_ui_nav_handshake(store), edits ? nav->settings_edits : NULL,
        edits ? nav->settings_edit_count : 0U, (enum mesh_ui_settings_section)section,
        mesh_ui_nav_open_channel(nav), nav->cursor[nav->screen], out);
}

uint32_t mesh_ui_nav_section_items(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   bool with_edits, struct mesh_ui_settings_item *items,
                                   uint32_t max) {
    const uint8_t section = mesh_ui_nav_open_section(nav);
    if (section == MESH_UI_SETTINGS_NO_SECTION) {
        return 0U;
    }
    const bool edits = mesh_ui_nav_edits_apply(nav, with_edits);
    return mesh_ui_settings_items(
        &store->settings, mesh_ui_nav_handshake(store), edits ? nav->settings_edits : NULL,
        edits ? nav->settings_edit_count : 0U, (enum mesh_ui_settings_section)section,
        mesh_ui_nav_open_channel(nav), items, max);
}

void mesh_ui_nav_open_radio_page(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                 enum mesh_ui_radio_page page) {
    nav->radio_devices_cursor = nav->cursor[MESH_UI_SCREEN_RADIO];
    nav->radio_page = (uint8_t)page;
    mesh_ui_nav_cursor_to_first_row(nav, store, MESH_UI_SCREEN_RADIO);
}

bool mesh_ui_nav_close_radio_page(struct mesh_ui_nav *nav) {
    if (nav->radio_page == MESH_UI_RADIO_PAGE_NONE) {
        return false;
    }
    nav->radio_page = MESH_UI_RADIO_PAGE_NONE;
    nav->cursor[MESH_UI_SCREEN_RADIO] = nav->radio_devices_cursor;
    return true;
}

static void mesh_ui_nav_edit_remove(struct mesh_ui_nav *nav, enum mesh_ui_setting_field field) {
    for (uint8_t i = 0; i < nav->settings_edit_count; ++i) {
        if (nav->settings_edits[i].field != (uint16_t)field) {
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
    if (base.kind == INKSTAND_FORM_TEXT) {
        same = (text != NULL && strcmp(base.text, text) == 0);
    } else if (base.kind == INKSTAND_FORM_KEY) {
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
        if (nav->settings_edits[i].field == (uint16_t)field) {
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
    slot->field = (uint16_t)field;
    slot->number = number;
    if (text != NULL) {
        snprintf(slot->text, sizeof slot->text, "%s", text);
    }
    return true;
}

void mesh_ui_nav_edits_clear(struct mesh_ui_nav *nav) {
    memset(nav->settings_edits, 0, sizeof nav->settings_edits);
    nav->settings_edit_count = 0U;
    nav->settings_discard_armed = false;
}

/* Opens the keyboard on a field's text. The Compose draft is parked until done or cancel. */
static void mesh_ui_nav_open_field_keyboard(struct mesh_ui_nav *nav,
                                            const struct mesh_ui_settings_item *item) {
    snprintf(nav->draft_saved, sizeof nav->draft_saved, "%s", nav->draft);
    snprintf(nav->draft, sizeof nav->draft, "%s", item->text);
    nav->keyboard_field = (uint16_t)item->field;
    nav->keyboard_open = true;
    inkcell_keyboard_reset(&nav->kb);
}

static bool mesh_ui_nav_settings_edit_item(struct mesh_ui_nav *nav,
                                           const struct mesh_ui_store *store,
                                           const struct mesh_ui_settings_item *row,
                                           enum inkcell_key key);

/* A, Left or Right on a row of an open section. Toggles flip, enums cycle, numbers step
   through their presets, text opens the keyboard. Read-only rows ignore the press. */
bool mesh_ui_nav_settings_edit_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   enum inkcell_key key) {
    struct mesh_ui_settings_item item;
    if (!mesh_ui_nav_settings_current(nav, store, true, &item) ||
        item.field == MESH_UI_FIELD_NONE) {
        return false;
    }
    const bool changed = mesh_ui_nav_settings_edit_item(nav, store, &item, key);
    /* A dimmed row takes the edit and says when it will count - see `inactive_note`. */
    if (changed && item.inactive && item.inactive_note != INKCELL_STR_NONE) {
        mesh_ui_nav_raise_toast(nav, inkcell_str(item.inactive_note));
    }
    return changed;
}

static bool mesh_ui_nav_settings_edit_item(struct mesh_ui_nav *nav,
                                           const struct mesh_ui_store *store,
                                           const struct mesh_ui_settings_item *row,
                                           enum inkcell_key key) {
    const struct mesh_ui_settings_item item = *row;
    const enum mesh_ui_setting_field field = item.field;
    const int delta = (key == INKCELL_KEY_LEFT) ? -1 : +1;
    switch (item.kind) {
    /* A flag is edited exactly as a toggle is - it carries 0 or 1 like one, and which bit of
       which word that ends up in is the write builder's business, not this one's. The kinds
       differ in how the row is *drawn*, which is the backend's. */
    case INKSTAND_FORM_TOGGLE:
    case INKSTAND_FORM_FLAG:
        return mesh_ui_nav_edit_set(nav, store, field, item.number != 0U ? 0U : 1U, NULL);
    case INKSTAND_FORM_ENUM: {
        /* The set of values is the row's, not the field's: a modem preset's depends on the
           region row above it, and the row was built after that row's edit. */
        const uint32_t count = mesh_ui_settings_enum_count(field);
        const uint32_t next = mesh_ui_settings_choice_step(item.choices, count, item.number, delta);
        if (count == 0U || next == item.number) {
            return false;
        }
        return mesh_ui_nav_edit_set(nav, store, field, next, NULL);
    }
    case INKSTAND_FORM_NUMBER: {
        const uint32_t next = mesh_ui_settings_number_step(field, item.number, delta);
        if (next == item.number || (item.ceiling != 0U && delta > 0 && next > item.ceiling)) {
            return false;
        }
        return mesh_ui_nav_edit_set(nav, store, field, next, NULL);
    }
    case INKSTAND_FORM_KEY:
        if (key == INKCELL_KEY_A) {
            mesh_ui_nav_open_field_keyboard(nav, &item); /* the key as hex */
            return true;
        }
        {
            /* Left/Right walk the choices the row offers - the same walk the enums take, over
               the same kind of set. A typed key is past the end of the stepped range and counts
               as "keep", so the walk starts there rather than off the end of it. */
            const uint32_t current = item.number >= MESH_UI_PSK_TYPED ? 0U : item.number;
            const uint32_t choice = mesh_ui_settings_choice_step(
                item.choices, (uint32_t)MESH_UI_PSK_TYPED, current, delta);
            return mesh_ui_nav_edit_set(nav, store, field, choice, NULL);
        }
    case INKSTAND_FORM_TEXT:
        if (key != INKCELL_KEY_A) {
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
    inkwell_str_copy(text, sizeof text, nav->draft);
    size_t cap = mesh_ui_settings_text_max(field);
    /* The buffer is measured from the field limits (mesh/ui/settings_text.def), so this is the
       bound holding rather than a cut: a field wide enough to need it would fail the test that
       walks the table. It stays because the cut below indexes `text` by `cap`. */
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
    /* A field whose value lives inside a larger string cannot carry that string's separator;
       drop it rather than sending something the read-back would split in two. Done before the
       edit is stored, so the row and the wire agree. */
    const char reserved = mesh_ui_settings_field_reserved_char(field);
    if (reserved != '\0') {
        size_t kept = 0U;
        for (size_t i = 0; text[i] != '\0'; ++i) {
            if (text[i] != reserved) {
                text[kept++] = text[i];
            }
        }
        text[kept] = '\0';
    }
    if (mesh_ui_settings_field_kind(field) == INKSTAND_FORM_KEY) {
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

/* A radio action carries the open section's pending edits for the same reason a save does:
   "Set fixed position" is a row that reads the three rows above it. The two forget rows sit in
   the same section and go through the same confirm sheet, but ask this client to drop its own
   roster rather than the radio to do anything - so they leave as their own type, and `number`
   says which of the two it was rather than which settings row. */
void mesh_ui_nav_fill_settings_action(const struct mesh_ui_nav *nav,
                                      enum mesh_ui_settings_action which,
                                      struct mesh_ui_action *action) {
    if (action == NULL) {
        return;
    }
    /*
     * The link the sheet is standing in front of, which is not a settings action at all: what
     * goes out is the characters the user typed, and the app parses them against the radio's
     * table as it stands at that moment. See MESH_UI_ACTION_IMPORT_CHANNELS in nav.h for why
     * the link travels rather than a parsed set.
     */
    if (which == MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS) {
        action->type = MESH_UI_ACTION_IMPORT_CHANNELS;
        action->section = mesh_ui_nav_open_section(nav);
        snprintf(action->text, sizeof action->text, "%s", nav->channel_url);
        return;
    }
    /* And the contact link, the same shape: the characters travel, the app parses them. */
    if (which == MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT) {
        action->type = MESH_UI_ACTION_IMPORT_CONTACT;
        action->section = mesh_ui_nav_open_section(nav);
        snprintf(action->text, sizeof action->text, "%s", nav->contact_url);
        return;
    }
    /*
     * Emptying a channel slot, which is a SET_CHANNEL and so travels as a save rather than as a
     * radio action: `channel` names the slot and `number` says this is the clearing sort, and
     * mesh_app_build_settings_write() answers both. Everything a save gets - the "Saving
     * channel n" toast, the ack or rejection tracked against it, the read-back - it gets too.
     *
     * No edits. Anything pending on this slot was typed into rows the write is about to erase,
     * so carrying them would mean sending a name on the way to clearing the name.
     */
    if (which == MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL) {
        action->type = MESH_UI_ACTION_SAVE_SETTINGS;
        action->section = mesh_ui_nav_open_section(nav);
        action->channel = nav->settings_channel;
        action->number = (uint32_t)which;
        action->edit_count = 0U;
        return;
    }
    if (mesh_ui_settings_action_is_forget(which)) {
        action->type = MESH_UI_ACTION_FORGET_NODES;
        action->section = mesh_ui_nav_open_section(nav);
        action->number = which == MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES ? 1U : 0U;
        return;
    }
    /*
     * The firmware installs leave as their own type for the same reason the forget rows do:
     * nothing about them is an admin request the radio answers. What the app has to do is
     * download half a megabyte, send one verb, and then talk to something that is no longer a
     * Meshtastic node at all - which is not a thing MESH_UI_ACTION_RADIO_ACTION can carry.
     * `number` says which bus, which is the one fact the nav knows and the app would otherwise
     * have to re-derive from a snapshot that has moved on since the press.
     */
    if (mesh_ui_settings_action_is_install_firmware(which)) {
        action->type = MESH_UI_ACTION_INSTALL_RADIO_FIRMWARE;
        action->section = mesh_ui_nav_open_section(nav);
        action->number = which == MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE ? 1U : 0U;
        return;
    }
    action->type = MESH_UI_ACTION_RADIO_ACTION;
    action->section = mesh_ui_nav_open_section(nav);
    action->number = (uint32_t)which;
    if (mesh_ui_nav_edits_apply(nav, true)) {
        action->edit_count = nav->settings_edit_count;
        memcpy(action->edits, nav->settings_edits, sizeof action->edits);
    }
}

bool mesh_ui_nav_confirm_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                             enum inkcell_key key, struct mesh_ui_action *action) {
    uint16_t subject = 0U;
    switch (inkstand_dialog_key(&nav->confirm, store != NULL ? store->focus : NULL,
                                (uint32_t)MESH_UI_FOCUS_DIALOG, key, &subject)) {
    case INKSTAND_DIALOG_MOVED:
    case INKSTAND_DIALOG_CANCELLED:
        return true;
    case INKSTAND_DIALOG_ACCEPTED:
        break;
    default:
        return false;
    }
    /* Two things stand behind this overlay: a section save, and an action row that keeps no state
       and so has no edits to carry - a radio one, or one of the two that ask this client to forget
       cached nodes. */
    if (subject == (uint16_t)MESH_UI_SETTINGS_ACTION_NONE) {
        mesh_ui_nav_fill_save(nav, action);
        return true;
    }
    const enum mesh_ui_settings_action confirmed = (enum mesh_ui_settings_action)subject;
    mesh_ui_nav_fill_settings_action(nav, confirmed, action);
    /*
     * The one confirmed row that takes itself off the screen: a cleared slot is an empty one, and
     * an empty slot is not offered the press. So the cursor is put back on a row that will still be
     * there, the way the press that leaves remote administration does.
     *
     * The cursor and *only* the cursor. Dropping the pending edits here as well would be this layer
     * deciding an outcome it does not know yet: whether the write was queued at all is the app's
     * answer, and mesh_app_save_settings() consumes the edits on a positive result and otherwise
     * says "edits kept". A clear confirmed with no link would have erased nothing and thrown away
     * the user's typing anyway.
     */
    if (confirmed == MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL) {
        nav->cursor[MESH_UI_SCREEN_SETTINGS] = 0U;
    }
    return true;
}

bool mesh_ui_nav_settings_back(struct mesh_ui_nav *nav) {
    /* A Radio tab page goes back to the cards, and holds no edits to drop on the way. */
    if (nav->screen == MESH_UI_SCREEN_RADIO) {
        return mesh_ui_nav_close_radio_page(nav);
    }
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
                                      enum inkcell_key key, struct mesh_ui_action *action,
                                      bool *handled) {
    *handled = true;
    /* The pending edits are the Settings tab's, so on a Radio tab page Y has nothing to save and
       B nothing to discard - and Left and Right, with no row to step, keep walking the tabs as
       they do on the cards under it. */
    const bool settings_tab = nav->screen == MESH_UI_SCREEN_SETTINGS;
    switch (key) {
    case INKCELL_KEY_LEFT:
    case INKCELL_KEY_RIGHT:
        if (!settings_tab) {
            *handled = false;
            return false;
        }
        return mesh_ui_nav_settings_edit_key(nav, store, key);
    /* A card at a time, where the section has cards. The d-pad walks rows and Left/Right are
       spoken for by the editor, so the crossing the cards draw is the shoulder pair's - see
       mesh_ui_nav_cursor_group(). A section with no groups refuses both, which is the same
       answer the screen gives by drawing no cards. */
    case INKCELL_KEY_L2:
        return mesh_ui_nav_cursor_group(nav, store, -1);
    case INKCELL_KEY_R2:
        return mesh_ui_nav_cursor_group(nav, store, +1);
    case INKCELL_KEY_Y:
        if (!settings_tab || nav->settings_edit_count == 0U) {
            return false;
        }
        if (mesh_ui_settings_section_needs_confirm(
                (enum mesh_ui_settings_section)nav->settings_section)) {
            /* Cancel under the cursor, so a repeated press changes nothing. */
            inkstand_dialog_open(&nav->confirm, (uint16_t)MESH_UI_SETTINGS_ACTION_NONE);
            return true;
        }
        mesh_ui_nav_fill_save(nav, action);
        return false; /* the app clears the edits once the write is queued */
    case INKCELL_KEY_B:
        if (settings_tab && nav->settings_edit_count > 0U && !nav->settings_discard_armed) {
            nav->settings_discard_armed = true; /* the footer now says "B again to discard" */
            return true;
        }
        return mesh_ui_nav_settings_back(nav);
    default:
        *handled = false;
        return false;
    }
}
