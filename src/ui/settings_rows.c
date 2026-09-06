#define _POSIX_C_SOURCE 200809L

/*
 * The rows a settings screen draws.
 *
 * Every section is built the same way: an item_list is filled by a build_* function, with the
 * pending edits layered over the radio's own values so a row shows what the user has typed
 * rather than what the radio last said. Rows are rebuilt on every draw rather than cached,
 * which is what keeps "the radio just told us something new" from needing an invalidation path.
 */

#include "settings_internal.h"

#include "mesh/core/radio_settings.h"
#include "mesh/core/updater.h"
#include "mesh/core/version.h"
#include "mesh/i18n/strings.h"
#include "mesh/utils/array.h"
#include "mesh/utils/text.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* "oKGio6Sl... (AES-128)", "default key", "no encryption"; `aes` names the size the way the
   channel list does, else it is plain bits. */
static void key_summary(const uint8_t *key, size_t len, bool aes, char *out, size_t out_len) {
    if (len == 0U) {
        snprintf(out, out_len, "%s",
                 mesh_str(aes ? MESH_STR_SETTINGS_KEY_NO_ENCRYPTION : MESH_STR_SETTINGS_KEY_NONE));
        return;
    }
    if (len == 1U) {
        if (key[0] == 1U) {
            snprintf(out, out_len, "%s", mesh_str(MESH_STR_SETTINGS_KEY_DEFAULT));
        } else {
            mesh_str_format(out, out_len, MESH_STR_SETTINGS_KEY_SIMPLE, (unsigned)key[0]);
        }
        return;
    }
    char text[48];
    mesh_ui_settings_key_text(key, len, text, sizeof text);
    mesh_str_format(out, out_len, aes ? MESH_STR_SETTINGS_KEY_AES : MESH_STR_SETTINGS_KEY_BITS,
                    text, (unsigned)(len * 8U));
}

/* ---- item builders ------------------------------------------------------------------------ */

struct item_list {
    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    uint32_t count;
    const struct mesh_ui_setting_edit *edits;
    size_t edit_count;
};

/*
 * Rows name their label with a catalog id; item_add_named() is the exception, for the two
 * labels that come off the wire rather than out of the catalog - a channel's own name and a
 * module's section title.
 */
static struct mesh_ui_settings_item *item_add_named(struct item_list *list, const char *label,
                                                    enum mesh_ui_setting_kind kind) {
    if (list->count >= MESH_UI_SETTINGS_ITEMS_MAX) {
        return NULL;
    }
    struct mesh_ui_settings_item *item = &list->items[list->count++];
    memset(item, 0, sizeof *item);
    snprintf(item->label, sizeof item->label, "%s", label);
    item->kind = kind;
    return item;
}

static struct mesh_ui_settings_item *item_add(struct item_list *list, enum mesh_str_id label,
                                              enum mesh_ui_setting_kind kind) {
    return item_add_named(list, mesh_str(label), kind);
}

static void item_text(struct item_list *list, enum mesh_str_id label,
                      enum mesh_ui_setting_kind kind, const char *value) {
    struct mesh_ui_settings_item *item = item_add(list, label, kind);
    if (item != NULL) {
        mesh_str_copy(item->value, sizeof item->value, value);
    }
}

/* The common case: both halves of the row are catalog entries. */
static void item_str(struct item_list *list, enum mesh_str_id label, enum mesh_ui_setting_kind kind,
                     enum mesh_str_id value) {
    item_text(list, label, kind, mesh_str(value));
}

/* A toggle the radio reports and nobody here can change. `number` is set for the same reason
   an editable toggle sets it: it is what a renderer drawing a switch rather than the words
   reads, and a read-only row that left it at 0 would draw every such row off. */
static void item_toggle(struct item_list *list, enum mesh_str_id label, bool value) {
    struct mesh_ui_settings_item *item = item_add(list, label, MESH_UI_SETTING_TOGGLE);
    if (item == NULL) {
        return;
    }
    mesh_str_copy(item->value, sizeof item->value,
                  mesh_str(value ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF));
    item->number = value ? 1U : 0U;
}

/* A group title. No value, no field, nothing happens when A lands on it. Headings are emitted
   unconditionally - never behind the group's own Enabled toggle - so an edit can never change
   the row count under the cursor. */
static void item_heading(struct item_list *list, enum mesh_str_id label) {
    item_add(list, label, MESH_UI_SETTING_HEADING);
}

/* "30s", "5m", "2h"; `zero` says what 0 means for this field ("off", "default"). */
static void format_seconds(char *out, size_t out_len, uint32_t seconds, enum mesh_str_id zero) {
    if (seconds == 0U) {
        snprintf(out, out_len, "%s", mesh_str(zero));
    } else if (seconds % 3600U == 0U) {
        mesh_str_format(out, out_len, MESH_STR_VALUE_HOURS, (unsigned)(seconds / 3600U));
    } else if (seconds % 60U == 0U) {
        mesh_str_format(out, out_len, MESH_STR_VALUE_MINUTES, (unsigned)(seconds / 60U));
    } else {
        mesh_str_format(out, out_len, MESH_STR_VALUE_SECONDS, (unsigned)seconds);
    }
}

/*
 * TEXT fields holding a credential. The row shows a fixed-width mask rather than the value -
 * a settings screen on a handheld is read over your shoulder, and the section is opened to
 * change one of the other rows far more often than to look at this one. The mask is a fixed
 * length so it does not leak how long the secret is.
 *
 * The keyboard still opens on the real text, exactly as the KEY rows do: it is the one place
 * a secret is revealed, and a credential you cannot see is a credential you cannot correct a
 * typo in. A predicate rather than another column in k_fields, the same way
 * mesh_ui_settings_section_needs_confirm() is a predicate.
 */
static bool field_is_secret(enum mesh_ui_setting_field field) {
    return field == MESH_UI_FIELD_MQTT_PASSWORD;
}

/* An editable row: the field's spec supplies label and kind; a pending edit replaces the
   radio's value and marks the row dirty. `text` is only read for TEXT fields. */
static void item_field(struct item_list *list, enum mesh_ui_setting_field field, uint32_t number,
                       const char *text) {
    const struct field_spec *spec = field_spec(field);
    struct mesh_ui_settings_item *item = item_add(list, spec->label, spec->kind);
    if (item == NULL) {
        return;
    }
    item->field = field;
    const struct mesh_ui_setting_edit *edit =
        mesh_ui_settings_find_edit(list->edits, list->edit_count, field);
    if (edit != NULL) {
        item->dirty = true;
        number = edit->number;
        text = edit->text;
    }
    item->number = number;
    switch (spec->kind) {
    case MESH_UI_SETTING_TOGGLE:
        snprintf(item->value, sizeof item->value, "%s",
                 mesh_str(number != 0U ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF));
        break;
    case MESH_UI_SETTING_ENUM:
        snprintf(item->value, sizeof item->value, "%s", mesh_ui_settings_enum_name(field, number));
        break;
    case MESH_UI_SETTING_NUMBER:
        /* A zero_label, where the field sets one, wins over the formatter: it is the field
           table's way of saying what 0 means for this row, and a formatter that also had an
           opinion silently overrode it - three Traffic management rows read "default" under a
           section whose whole convention is that 0 is off. */
        if (number == 0U && spec->zero_label != MESH_STR_NONE) {
            snprintf(item->value, sizeof item->value, "%s", mesh_str(spec->zero_label));
        } else if (spec->format != NULL) {
            spec->format(number, item->value, sizeof item->value);
        } else {
            format_seconds(item->value, sizeof item->value, number,
                           spec->zero_label != MESH_STR_NONE ? spec->zero_label
                                                             : MESH_STR_VALUE_ZERO);
        }
        break;
    case MESH_UI_SETTING_TEXT:
        snprintf(item->text, sizeof item->text, "%s", text != NULL ? text : "");
        if (item->text[0] == '\0') {
            snprintf(item->value, sizeof item->value, "%s", mesh_str(MESH_STR_SETTINGS_TEXT_EMPTY));
        } else if (field_is_secret(field)) {
            snprintf(item->value, sizeof item->value, "%s",
                     mesh_str(MESH_STR_SETTINGS_TEXT_SECRET));
        } else {
            mesh_str_copy(item->value, sizeof item->value, item->text);
        }
        break;
    default:
        break;
    }
}

/* A KEY row. `key`/`len` is the radio's current key; an edit is a choice, or typed text. The
   text carried is what the keyboard should open on: the typed text if there is one, else the
   current key as base64 (an explicit reveal, never shown in the row). */
static void item_key_field(struct item_list *list, enum mesh_ui_setting_field field,
                           const uint8_t *key, size_t len) {
    const struct field_spec *spec = field_spec(field);
    const bool aes = (field == MESH_UI_FIELD_CHANNEL_KEY);
    struct mesh_ui_settings_item *item = item_add(list, spec->label, spec->kind);
    if (item == NULL) {
        return;
    }
    item->field = field;
    mesh_ui_settings_key_text(key, len, item->text, sizeof item->text);
    const struct mesh_ui_setting_edit *edit =
        mesh_ui_settings_find_edit(list->edits, list->edit_count, field);
    item->number = edit != NULL ? edit->number : (uint32_t)MESH_UI_PSK_KEEP;
    item->dirty = edit != NULL;
    switch ((enum mesh_ui_psk_choice)item->number) {
    case MESH_UI_PSK_DEFAULT:
        snprintf(item->value, sizeof item->value, "%s", mesh_str(MESH_STR_SETTINGS_KEY_DEFAULT));
        break;
    case MESH_UI_PSK_RANDOM_128:
        snprintf(item->value, sizeof item->value, "%s", mesh_str(MESH_STR_SETTINGS_KEY_NEW_AES128));
        break;
    case MESH_UI_PSK_RANDOM_256:
        snprintf(item->value, sizeof item->value, "%s",
                 mesh_str(aes ? MESH_STR_SETTINGS_KEY_NEW_AES256 : MESH_STR_SETTINGS_KEY_NEW_KEY));
        break;
    case MESH_UI_PSK_NONE:
        snprintf(item->value, sizeof item->value, "%s",
                 mesh_str(aes ? MESH_STR_SETTINGS_KEY_NO_ENCRYPTION : MESH_STR_SETTINGS_KEY_CLEAR));
        break;
    case MESH_UI_PSK_TYPED: {
        uint8_t typed[MESH_UI_PSK_MAX];
        size_t typed_len = 0U;
        snprintf(item->text, sizeof item->text, "%s", edit->text);
        if (mesh_ui_settings_key_parse(edit->text, typed, sizeof typed, &typed_len)) {
            key_summary(typed, typed_len, aes, item->value, sizeof item->value);
        } else {
            snprintf(item->value, sizeof item->value, "%s",
                     mesh_str(MESH_STR_SETTINGS_KEY_INVALID));
        }
        break;
    }
    case MESH_UI_PSK_KEEP:
    default:
        key_summary(key, len, aes, item->value, sizeof item->value);
        break;
    }
}

/* Keys are shown as a short fingerprint: enough to compare against the phone app's view,
   not enough to leak the key to someone reading over your shoulder. */
static void item_key(struct item_list *list, enum mesh_str_id label, const uint8_t *key,
                     size_t len) {
    struct mesh_ui_settings_item *item = item_add(list, label, MESH_UI_SETTING_KEY);
    if (item == NULL) {
        return;
    }
    if (len == 0U) {
        snprintf(item->value, sizeof item->value, "%s", mesh_str(MESH_STR_SETTINGS_KEY_NONE));
        return;
    }
    size_t shown = len < 4U ? len : 4U;
    char hex[9] = {0};
    for (size_t i = 0; i < shown; ++i) {
        snprintf(hex + 2U * i, sizeof hex - 2U * i, "%02x", key[i]);
    }
    mesh_str_format(item->value, sizeof item->value, MESH_STR_SETTINGS_KEY_FINGERPRINT, hex,
                    (unsigned)len);
}

/* An ACTION row: drawn like an editable one and activated with A, carrying what it does in
   `number` so the nav can raise the action without knowing about updates. */
static void item_action_named(struct item_list *list, const char *label, const char *value,
                              enum mesh_ui_settings_action action) {
    struct mesh_ui_settings_item *item = item_add_named(list, label, MESH_UI_SETTING_ACTION);
    if (item != NULL) {
        snprintf(item->value, sizeof item->value, "%s", value);
        item->number = (uint32_t)action;
    }
}

static void item_action(struct item_list *list, enum mesh_str_id label, const char *value,
                        enum mesh_ui_settings_action action) {
    item_action_named(list, mesh_str(label), value, action);
}

/* One of the two rows that drop cached nodes. The value column is the count the press would
   remove, so a row with nothing to remove is a fact rather than a press that does nothing -
   and the two can never disagree, because the count came through the same predicate the
   forget itself uses. */
static void forget_row(struct item_list *list, enum mesh_str_id label, uint32_t forgettable,
                       enum mesh_ui_settings_action action) {
    if (forgettable == 0U) {
        item_str(list, label, MESH_UI_SETTING_INFO, MESH_STR_ACTION_NOTHING_TO_DROP);
        return;
    }
    char value[MESH_UI_SETTINGS_VALUE_MAX];
    mesh_str_format_plural(value, sizeof value, MESH_STR_ACTION_FORGET_COUNT_ONE, forgettable,
                           forgettable);
    item_action(list, label, value, action);
}

/* An action the radio has to be reachable for. Without a link it becomes the same row saying
   why, so the section keeps its shape whatever the transport is doing. */
static void item_radio_action(struct item_list *list, enum mesh_str_id label,
                              enum mesh_ui_settings_action action, bool connected) {
    if (connected) {
        item_action(list, label, mesh_str(MESH_STR_COMMON_PRESS_A), action);
    } else {
        item_str(list, label, MESH_UI_SETTING_INFO, MESH_STR_SETTINGS_NOT_CONNECTED);
    }
}

/*
 * About: what this client is, and the self-update rows. The only section that renders with no
 * radio connected, and the only one whose values come from the app rather than the air.
 *
 * Every row that responds to A carries a verb in its value column, not a bare "A". The button
 * hint on its own read as data - "Check for updates > A" looks like a setting whose value is
 * the letter A - and left no clue that anything would happen.
 *
 * The update rows are deliberately a check and a separate install rather than one button. The
 * install downloads and replaces the running binary, so it is worth a second, deliberate press
 * once the user can see which version they are about to move to.
 */
static void build_about(const struct mesh_ui_settings *s, struct item_list *list) {
    const struct mesh_ui_client_info *client = &s->client;
    item_text(list, MESH_STR_ABOUT_VERSION, MESH_UI_SETTING_INFO,
              client->version[0] != '\0' ? client->version
                                         : mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT));
    if (client->backend[0] != '\0') {
        item_text(list, MESH_STR_ABOUT_UI_BACKEND, MESH_UI_SETTING_INFO, client->backend);
    }
    if (client->data_dir[0] != '\0') {
        item_text(list, MESH_STR_ABOUT_DATA, MESH_UI_SETTING_INFO, client->data_dir);
    }
    /* A fact, not a switch, while the build ships one language: it says which one the catalog
       resolved to, so MESHCLIENT_LANG naming a language this build does not have shows up here
       rather than as a screen that is silently still in English. It is where the picker goes
       when there is something to pick - see docs/i18n.md. */
    if (client->language_name[0] != '\0') {
        item_text(list, MESH_STR_ABOUT_LANGUAGE, MESH_UI_SETTING_INFO, client->language_name);
    }

    /*
     * The look. Above the update rows on purpose: those return early in three places - no
     * updater, a check in flight, an install ready - and a row placed after them would
     * disappear exactly when somebody sat in the sun wanted it.
     *
     * Cycling rather than a submenu because the screen is its own preview: pressing A steps to
     * the next theme and the frame it draws is the answer. Held as a fact when the environment
     * named one, the same way the dev-updates switch is.
     */
    if (client->theme_name[0] != '\0' || client->theme[0] != '\0') {
        const char *const name = client->theme_name[0] != '\0' ? client->theme_name : client->theme;
        if (client->theme_from_env) {
            /* The note goes in the label, not the value. The value column is about eighteen
               cells at the device scale, so "High contrast (environment)" clipped to "High
               contrast (env" - a note that reads as a bug. The label column has room for the
               note whatever the theme is called, and the value stays the plain name. */
            item_text(list, MESH_STR_ABOUT_THEME_ENV, MESH_UI_SETTING_INFO, name);
        } else {
            item_action(list, MESH_STR_ABOUT_THEME, name, MESH_UI_SETTINGS_ACTION_CYCLE_THEME);
        }
    }

    if (!client->update_supported) {
        item_text(list, MESH_STR_ABOUT_UPDATES, MESH_UI_SETTING_INFO,
                  client->update_message[0] != '\0' ? client->update_message
                                                    : mesh_str(MESH_STR_ABOUT_UPDATES_UNAVAILABLE));
        return;
    }

    /*
     * The channel is a setting, so its value column is the setting rather than a verb; that it
     * responds to A is what the marker says. It comes before the status because it decides
     * which question a check will ask.
     *
     * While a child is running it drops to a plain fact: switching channel mid-download would
     * pull the asset out from under it, so the updater refuses, and a row that refuses is
     * worse than one that never invited the press.
     */
    const char *const channel = client->update_channel[0] != '\0'
                                    ? client->update_channel
                                    : mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
    if (client->update_busy) {
        item_text(list, MESH_STR_ABOUT_UPDATE_CHANNEL, MESH_UI_SETTING_INFO, channel);
    } else {
        item_action(list, MESH_STR_ABOUT_UPDATE_CHANNEL, channel,
                    MESH_UI_SETTINGS_ACTION_CYCLE_UPDATE_CHANNEL);
    }

    /*
     * The dev-updates switch, on a build that is not a release. It exists because the guard it
     * lifts is the only thing standing between a hand-deployed build and the install path, and
     * the alternative way in - an environment variable - needs a computer and an ssh session,
     * which is exactly what a handheld does not have. A release build never sees this row:
     * there is no guard on it to lift.
     */
    if (!client->update_is_release) {
        if (client->update_allow_dev_from_env) {
            /* Held on by MESHCLIENT_UPDATE_ALLOW_DEV. Shown as a fact rather than a switch,
               because a toggle that sprang back would look broken. */
            item_str(list, MESH_STR_ABOUT_DEV_UPDATES, MESH_UI_SETTING_INFO,
                     MESH_STR_ABOUT_DEV_UPDATES_ENV);
        } else if (client->update_busy) {
            item_str(list, MESH_STR_ABOUT_DEV_UPDATES, MESH_UI_SETTING_INFO,
                     client->update_allow_dev ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF);
        } else {
            item_action(
                list, MESH_STR_ABOUT_DEV_UPDATES,
                mesh_str(client->update_allow_dev ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF),
                MESH_UI_SETTINGS_ACTION_TOGGLE_DEV_UPDATES);
        }
    }

    const enum mesh_update_state state = (enum mesh_update_state)client->update_state;
    item_text(list, MESH_STR_ABOUT_UPDATE_STATUS, MESH_UI_SETTING_INFO,
              client->update_message[0] != '\0' ? client->update_message
                                                : mesh_update_state_name(state));

    /* While a child is running neither update row does anything, so both say so rather than
       inviting a press that would be swallowed. */
    if (client->update_busy) {
        item_str(list, MESH_STR_ABOUT_WORKING, MESH_UI_SETTING_INFO,
                 state == MESH_UPDATE_DOWNLOADING ? MESH_STR_ABOUT_WORKING_DOWNLOAD
                                                  : MESH_STR_ABOUT_WORKING_CHECK);
        return;
    }
    if (state == MESH_UPDATE_READY) {
        item_str(list, MESH_STR_ABOUT_INSTALLED, MESH_UI_SETTING_INFO,
                 MESH_STR_ABOUT_INSTALLED_RELAUNCH);
        return;
    }

    item_action(list, MESH_STR_ABOUT_CHECK_UPDATES, mesh_str(MESH_STR_COMMON_PRESS_A),
                MESH_UI_SETTINGS_ACTION_CHECK_UPDATE);
    if (state == MESH_UPDATE_AVAILABLE) {
        /* The version goes in the label so the value column can say how to act on it: the row
           the user has to find is the one that names what it will install. The label is
           bounded by its own column, not by what the release named itself. */
        char label[MESH_UI_SETTINGS_LABEL_MAX];
        mesh_str_format(label, sizeof label, MESH_STR_ABOUT_INSTALL_VERSION,
                        (int)(sizeof label - 9U), client->update_latest);
        item_action_named(list, label, mesh_str(MESH_STR_COMMON_PRESS_A),
                          MESH_UI_SETTINGS_ACTION_INSTALL_UPDATE);
    } else if (!client->update_can_install) {
        /* Nothing here will offer an install, so say so once - and name the row that changes
           it, rather than leaving the user hunting for one that is never coming. */
        item_str(list, MESH_STR_ABOUT_INSTALLING, MESH_UI_SETTING_INFO, MESH_STR_ABOUT_TURN_ON_DEV);
    }
}

/*
 * About radio: what the radio *is*, as About MeshClient is what this client is. Every row is
 * read-only, and that is the whole distinction the two names carry - a row that can be changed
 * lives in the section that owns it, never here.
 *
 * Which is why the LoRa region is not on this screen although it is the one radio fact people
 * look for. It is editable, so it belongs to LoRa, and a read-only copy here is a row that
 * answers "what region is this radio on" in the one place that cannot answer "change it".
 */
static void build_radio(const struct mesh_ui_settings *s, const struct mesh_ui_handshake_state *hs,
                        struct item_list *list) {
    char buffer[48];
    if (s->has_metadata) {
        item_text(list, MESH_STR_RADIO_FIRMWARE, MESH_UI_SETTING_INFO,
                  s->firmware_version[0] != '\0' ? s->firmware_version
                                                 : mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT));
        item_text(list, MESH_STR_RADIO_HARDWARE, MESH_UI_SETTING_INFO,
                  mesh_radio_hw_model_name(s->hw_model, buffer, sizeof buffer));
    }
    if (hs != NULL && hs->has_my_info) {
        mesh_str_format(buffer, sizeof buffer, MESH_STR_NODE_VAL_USER_ID_HEX, hs->my_info.node_num);
        item_text(list, MESH_STR_RADIO_NODE_NUMBER, MESH_UI_SETTING_INFO, buffer);
        mesh_str_format(buffer, sizeof buffer, MESH_STR_VALUE_PLAIN, hs->my_info.reboot_count);
        item_text(list, MESH_STR_RADIO_REBOOTS, MESH_UI_SETTING_INFO, buffer);
    }
    if (s->has_metadata) {
        snprintf(buffer, sizeof buffer, "%s%s%s%s",
                 s->has_bluetooth_radio ? mesh_str(MESH_STR_RADIO_CAP_BLE) : "",
                 s->has_wifi ? mesh_str(MESH_STR_RADIO_CAP_WIFI) : "",
                 s->has_ethernet ? mesh_str(MESH_STR_RADIO_CAP_ETHERNET) : "",
                 s->has_pkc ? mesh_str(MESH_STR_RADIO_CAP_PKC) : "");
        item_text(list, MESH_STR_RADIO_CAPABILITIES, MESH_UI_SETTING_INFO,
                  buffer[0] != '\0' ? buffer : mesh_str(MESH_STR_RADIO_CAP_NONE));
        item_toggle(list, MESH_STR_RADIO_CAN_SHUT_DOWN, s->can_shutdown);
    }
    if (s->admin_ok) {
        mesh_str_format(buffer, sizeof buffer, MESH_STR_RADIO_ADMIN_OK, (unsigned)s->admin_replies,
                        s->write_pending ? mesh_str(MESH_STR_RADIO_ADMIN_SAVING)
                        : s->admin_busy  ? mesh_str(MESH_STR_RADIO_ADMIN_REFRESHING)
                                         : "");
    } else {
        snprintf(
            buffer, sizeof buffer, "%s",
            mesh_str(s->admin_busy ? MESH_STR_RADIO_ADMIN_WAITING : MESH_STR_RADIO_ADMIN_NO_REPLY));
    }
    item_text(list, MESH_STR_RADIO_ADMIN_SESSION, MESH_UI_SETTING_INFO, buffer);
}

static void build_user(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_USER_LONG_NAME, 0U, s->long_name);
    item_field(list, MESH_UI_FIELD_USER_SHORT_NAME, 0U, s->short_name);
    item_field(list, MESH_UI_FIELD_USER_LICENSED, s->is_licensed ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_USER_UNMESSAGEABLE, s->is_unmessagable ? 1U : 0U, NULL);
}

static void build_device(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_DEVICE_ROLE, s->role, NULL);
    item_field(list, MESH_UI_FIELD_DEVICE_TZDEF, 0U, s->tzdef);
    item_field(list, MESH_UI_FIELD_DEVICE_REBROADCAST, s->rebroadcast_mode, NULL);
    item_field(list, MESH_UI_FIELD_DEVICE_NODEINFO_SECS, s->node_info_broadcast_secs, NULL);
    /* The protobuf field is led_heartbeat_disabled; the row is the plain statement. */
    item_field(list, MESH_UI_FIELD_DEVICE_LED_HEARTBEAT, s->led_heartbeat_disabled ? 0U : 1U, NULL);
    item_field(list, MESH_UI_FIELD_DEVICE_DOUBLE_TAP, s->double_tap_as_button_press ? 1U : 0U,
               NULL);
}

static void build_display(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_DISPLAY_SCREEN_ON, s->screen_on_secs, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_CAROUSEL, s->carousel_secs, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_COMPASS, s->compass_orientation, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_12H, s->use_12h_clock ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_UNITS, s->units, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_FLIP, s->flip_screen ? 1U : 0U, NULL);
}

static void build_lora(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_LORA_REGION, s->region, NULL);
    item_field(list, MESH_UI_FIELD_LORA_USE_PRESET, s->use_preset ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_LORA_PRESET, s->modem_preset, NULL);
    /* The manual trio only applies with the preset off; they stay listed so the row count
       does not move under the cursor as the toggle is edited. */
    item_field(list, MESH_UI_FIELD_LORA_BANDWIDTH, s->bandwidth, NULL);
    item_field(list, MESH_UI_FIELD_LORA_SPREAD, s->spread_factor, NULL);
    item_field(list, MESH_UI_FIELD_LORA_CODING, s->coding_rate, NULL);
    item_field(list, MESH_UI_FIELD_LORA_HOPS, s->hop_limit, NULL);
    item_field(list, MESH_UI_FIELD_LORA_TX_ENABLED, s->tx_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_LORA_TX_POWER, (uint32_t)(uint8_t)s->tx_power, NULL);
    item_field(list, MESH_UI_FIELD_LORA_IGNORE_MQTT, s->ignore_mqtt ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_LORA_OK_TO_MQTT, s->config_ok_to_mqtt ? 1U : 0U, NULL);
}

static void build_bluetooth(const struct mesh_ui_settings *s, struct item_list *list) {
    char pin[8];
    snprintf(pin, sizeof pin, "%06u", (unsigned)(s->fixed_pin % 1000000U));
    item_field(list, MESH_UI_FIELD_BT_ENABLED, s->bluetooth_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_BT_MODE, s->pairing_mode, NULL);
    item_field(list, MESH_UI_FIELD_BT_PIN, 0U, pin);
}

static void channel_label(uint8_t index, const char *name, char *out, size_t out_len) {
    mesh_str_format(out, out_len, MESH_STR_CHANNELS_SLOT, (unsigned)index,
                    name[0] != '\0' ? name
                                    : mesh_str(index == 0U ? MESH_STR_ENUM_CHANNEL_PRIMARY
                                                           : MESH_STR_COMMON_UNKNOWN_SHORT));
}

static void channel_summary(uint8_t role, uint8_t psk_len, bool uplink, bool downlink, char *out,
                            size_t out_len) {
    const char *key = mesh_str(psk_len == 0U    ? MESH_STR_CHANNELS_KEY_NONE
                               : psk_len == 1U  ? MESH_STR_CHANNELS_KEY_DEFAULT
                               : psk_len == 16U ? MESH_STR_CHANNELS_KEY_AES128
                               : psk_len == 32U ? MESH_STR_CHANNELS_KEY_AES256
                                                : MESH_STR_CHANNELS_KEY_ODD);
    mesh_str_format(
        out, out_len, MESH_STR_CHANNELS_SUMMARY,
        mesh_str(role == 1U ? MESH_STR_CHANNELS_ROLE_PRIMARY : MESH_STR_CHANNELS_ROLE_SECONDARY),
        key, mesh_str(uplink ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF),
        mesh_str(downlink ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF));
}

/* The channel list. With the radio's full table held every slot is listed, disabled ones
   included, and A opens it: that is how a channel is added (set up an empty slot) or removed
   (set its role to Disabled). Without the table only the handshake summary of the enabled
   slots is shown, read-only. */
static void build_channels(const struct mesh_ui_settings *s,
                           const struct mesh_ui_handshake_state *hs, struct item_list *list) {
    char label[MESH_UI_SETTINGS_LABEL_MAX];
    if (s->has_channels) {
        for (uint32_t i = 0; i < MESH_UI_MAX_CHANNELS; ++i) {
            const struct mesh_ui_channel_detail *channel = &s->channels[i];
            if (!channel->present) {
                continue;
            }
            if (channel->role == 0U) {
                mesh_str_format(label, sizeof label, MESH_STR_CHANNELS_SLOT_EMPTY,
                                (unsigned)channel->index);
            } else {
                channel_label(channel->index, channel->name, label, sizeof label);
            }
            struct mesh_ui_settings_item *item =
                item_add_named(list, label, MESH_UI_SETTING_ACTION);
            if (item == NULL) {
                continue;
            }
            item->number = channel->index;
            if (channel->role == 0U) {
                snprintf(item->value, sizeof item->value, "%s", mesh_str(MESH_STR_CHANNELS_SET_UP));
            } else {
                channel_summary(channel->role, channel->psk_len, channel->uplink_enabled,
                                channel->downlink_enabled, item->value, sizeof item->value);
            }
        }
    } else if (hs != NULL) {
        for (uint32_t i = 0; i < hs->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
            const struct mesh_ui_channel *channel = &hs->channels[i];
            if (channel->role == 0U) {
                continue;
            }
            channel_label(channel->index, channel->name, label, sizeof label);
            struct mesh_ui_settings_item *item = item_add_named(list, label, MESH_UI_SETTING_INFO);
            if (item != NULL) {
                channel_summary(channel->role, channel->psk_len, channel->uplink_enabled,
                                channel->downlink_enabled, item->value, sizeof item->value);
                item->number = channel->index;
            }
        }
    }
    if (list->count == 0U) {
        item_str(list, MESH_STR_SETTINGS_CHANNELS_ROW, MESH_UI_SETTING_INFO,
                 MESH_STR_CHANNELS_NONE_KNOWN);
    }
}

/* One channel's rows. The primary slot's role is shown but not offered: a mesh with two
   primaries or none is not something to reach by accident. */
static void build_channel(const struct mesh_ui_settings *s, uint8_t slot, struct item_list *list) {
    if (slot >= MESH_UI_MAX_CHANNELS || !s->channels[slot].present) {
        return;
    }
    const struct mesh_ui_channel_detail *channel = &s->channels[slot];
    item_field(list, MESH_UI_FIELD_CHANNEL_NAME, 0U, channel->name);
    if (channel->role == 1U) {
        item_str(list, MESH_STR_SETTINGS_ROLE_ROW, MESH_UI_SETTING_INFO,
                 MESH_STR_ENUM_CHANNEL_PRIMARY);
    } else {
        item_field(list, MESH_UI_FIELD_CHANNEL_ROLE, channel->role == 2U ? 1U : 0U, NULL);
    }
    item_key_field(list, MESH_UI_FIELD_CHANNEL_KEY, channel->psk, channel->psk_len);
    item_field(list, MESH_UI_FIELD_CHANNEL_UPLINK, channel->uplink_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_CHANNEL_DOWNLINK, channel->downlink_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_CHANNEL_POSITION, channel->position_precision, NULL);
}

int mesh_ui_settings_channel_at_row(const struct mesh_ui_settings *settings,
                                    const struct mesh_ui_handshake_state *handshake, uint32_t row) {
    struct mesh_ui_settings_item item;
    if (settings == NULL || !settings->has_channels ||
        !mesh_ui_settings_item(settings, handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, row, &item) ||
        item.kind != MESH_UI_SETTING_ACTION) {
        return -1;
    }
    return (int)item.number;
}

static void build_security(const struct mesh_ui_settings *s, struct item_list *list) {
    item_key(list, MESH_STR_SETTINGS_PUBLIC_KEY_ROW, s->public_key, s->public_key_len);
    item_key_field(list, MESH_UI_FIELD_SECURITY_PRIVATE_KEY, s->private_key, s->private_key_len);
    for (unsigned i = 0; i < 3U; ++i) {
        item_key_field(list, (enum mesh_ui_setting_field)(MESH_UI_FIELD_SECURITY_ADMIN_KEY_0 + i),
                       s->admin_keys[i], s->admin_key_lens[i]);
    }
    item_field(list, MESH_UI_FIELD_SECURITY_SIGNATURE_POLICY, s->packet_signature_policy, NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_MANAGED, s->is_managed ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_ADMIN_CHANNEL, s->admin_channel_enabled ? 1U : 0U,
               NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_SERIAL, s->serial_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_DEBUG_LOG, s->debug_log_api_enabled ? 1U : 0U, NULL);
}

static void build_position(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_POSITION_GPS_MODE, s->gps_mode, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_BROADCAST_SECS, s->position_broadcast_secs, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_SMART, s->position_broadcast_smart_enabled ? 1U : 0U,
               NULL);
    /* Listed whatever the toggle says, so the row count does not move under the cursor while
       smart broadcast is being turned on and off; the same rule the LoRa trio follows. */
    item_field(list, MESH_UI_FIELD_POSITION_SMART_DISTANCE, s->smart_minimum_distance, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_SMART_INTERVAL, s->smart_minimum_interval_secs, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_GPS_INTERVAL, s->gps_update_interval, NULL);

    /*
     * Fixed position. The flag is shown rather than offered: the firmware sets it itself as
     * part of set_fixed_position, and a toggle here could only turn it on with no coordinates
     * behind it - which leaves the radio broadcasting whatever it last had.
     *
     * The three coordinate rows are edited like any other text, but they are not saved with
     * the section: Y writes PositionConfig, and these go out through the action row below
     * them. They are pre-filled with where the radio says it is, so a fix that came from a
     * GPS can be pinned down by opening the section and pressing one row.
     */
    item_toggle(list, MESH_STR_SETTINGS_FIXED_POSITION, s->fixed_position);
    char coord[MESH_UI_SETTINGS_VALUE_MAX];
    mesh_ui_settings_coord_text(s->has_own_position ? s->own_latitude_i : 0, coord, sizeof coord);
    item_field(list, MESH_UI_FIELD_POSITION_LATITUDE, 0U, coord);
    mesh_ui_settings_coord_text(s->has_own_position ? s->own_longitude_i : 0, coord, sizeof coord);
    item_field(list, MESH_UI_FIELD_POSITION_LONGITUDE, 0U, coord);
    snprintf(coord, sizeof coord, "%d", s->has_own_altitude ? (int)s->own_altitude : 0);
    item_field(list, MESH_UI_FIELD_POSITION_ALTITUDE, 0U, coord);
    item_action(list, MESH_STR_SETTINGS_SET_FIXED_POS, mesh_str(MESH_STR_COMMON_PRESS_A),
                MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION);
    /* Only offered when there is one to clear; the row would otherwise do nothing twice. */
    if (s->fixed_position) {
        item_action(list, MESH_STR_SETTINGS_CLEAR_FIXED_POS, mesh_str(MESH_STR_COMMON_PRESS_A),
                    MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION);
    }
}

static void build_power(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_POWER_SAVING, s->is_power_saving ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_POWER_LS_SECS, s->ls_secs, NULL);
    item_field(list, MESH_UI_FIELD_POWER_MIN_WAKE, s->min_wake_secs, NULL);
    item_field(list, MESH_UI_FIELD_POWER_WAIT_BT, s->wait_bluetooth_secs, NULL);
    item_field(list, MESH_UI_FIELD_POWER_SHUTDOWN, s->on_battery_shutdown_after_secs, NULL);
}

/*
 * The Modules list: every ModuleConfig variant this client keeps, with its enabled state as
 * the value, and A on a row opening that module's own section.
 *
 * ACTION rows carrying the target section in `number`, which is the shape the channel list
 * already uses - the nav intercepts A on them ahead of the radio-action handling rather than
 * either list needing a screen of its own. A module the radio has not sent says so instead of
 * being hidden: "which of these has not arrived" is most of what this screen is for.
 */
static void build_modules(const struct mesh_ui_settings *s,
                          const struct mesh_ui_handshake_state *hs, struct item_list *list) {
    const uint32_t count = mesh_ui_settings_module_count();
    for (uint32_t i = 0; i < count; ++i) {
        const enum mesh_ui_settings_section section = mesh_ui_settings_module_at(i);
        struct mesh_ui_settings_item *item =
            item_add_named(list, mesh_ui_settings_section_name(section), MESH_UI_SETTING_ACTION);
        if (item == NULL) {
            continue;
        }
        item->number = (uint32_t)section;
        if (!mesh_ui_settings_section_loaded(s, hs, section)) {
            mesh_str_copy(item->value, sizeof item->value, mesh_str(MESH_STR_SETTINGS_NOT_LOADED));
            continue;
        }
        bool enabled = false;
        switch (section) {
        case MESH_UI_SETTINGS_MQTT:
            enabled = s->mqtt_enabled;
            break;
        case MESH_UI_SETTINGS_STORE_FORWARD:
            enabled = s->store_forward_enabled;
            break;
        case MESH_UI_SETTINGS_TELEMETRY:
            /* Telemetry has no single enabled flag; it is on when it is reporting anything. */
            enabled = s->device_telemetry_enabled || s->environment_measurement_enabled ||
                      s->air_quality_enabled || s->power_measurement_enabled ||
                      s->health_measurement_enabled;
            break;
        case MESH_UI_SETTINGS_NEIGHBOR_INFO:
            enabled = s->neighbor_info_enabled;
            break;
        case MESH_UI_SETTINGS_RANGE_TEST:
            enabled = s->range_test_enabled;
            break;
        case MESH_UI_SETTINGS_PAXCOUNTER:
            enabled = s->paxcounter_enabled;
            break;
        case MESH_UI_SETTINGS_AMBIENT:
            enabled = s->ambient_led_state;
            break;
        case MESH_UI_SETTINGS_STATUS_MESSAGE:
            /* No enabled flag: the module is doing something exactly when there is a status. */
            enabled = s->status_message[0] != '\0';
            break;
        case MESH_UI_SETTINGS_TAK:
            /* Nor here - TAKConfig is two enums with no switch. Unspecifed/Unspecifed is the
               untouched state, which is as close to "off" as this module gets. */
            enabled = s->tak_team != 0U || s->tak_role != 0U;
            break;
        case MESH_UI_SETTINGS_DETECTION:
            enabled = s->detection_enabled;
            break;
        case MESH_UI_SETTINGS_EXT_NOTIFICATION:
            enabled = s->extnotif_enabled;
            break;
        case MESH_UI_SETTINGS_TRAFFIC:
            /* No enabled flag: upstream removed the bool toggles for "non-zero implies
               enabled", so this module is on when any of its five rows is set. */
            enabled = s->traffic_position_min_interval_secs != 0U ||
                      s->traffic_nodeinfo_max_hops != 0U ||
                      s->traffic_rate_limit_window_secs != 0U ||
                      s->traffic_rate_limit_max_packets != 0U ||
                      s->traffic_unknown_packet_threshold != 0U;
            break;
        default:
            break;
        }
        mesh_str_copy(item->value, sizeof item->value,
                      mesh_str(enabled ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF));
    }
}

static void build_mqtt(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_MQTT_ENABLED, s->mqtt_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_MQTT_ADDRESS, 0U, s->mqtt_address);
    item_field(list, MESH_UI_FIELD_MQTT_USERNAME, 0U, s->mqtt_username);
    item_field(list, MESH_UI_FIELD_MQTT_PASSWORD, 0U, s->mqtt_password);
    item_field(list, MESH_UI_FIELD_MQTT_ROOT, 0U, s->mqtt_root);
    item_field(list, MESH_UI_FIELD_MQTT_ENCRYPTION, s->mqtt_encryption_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_MQTT_TLS, s->mqtt_tls_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_MQTT_MAP_REPORTING, s->mqtt_map_reporting_enabled ? 1U : 0U,
               NULL);
    /* MapReportSettings, the one submessage in this section. Listed under the toggle that
       decides whether the radio reads them at all, and listed whether or not it is on - the
       heading rule. */
    item_heading(list, MESH_STR_HEAD_MAP_REPORT);
    item_field(list, MESH_UI_FIELD_MQTT_MAP_INTERVAL, s->mqtt_map_publish_interval_secs, NULL);
    item_field(list, MESH_UI_FIELD_MQTT_MAP_PRECISION, s->mqtt_map_position_precision, NULL);
    item_field(list, MESH_UI_FIELD_MQTT_MAP_LOCATION, s->mqtt_map_should_report_location ? 1U : 0U,
               NULL);
    /*
     * The one row here that stays read-only. With proxying on, the radio stops talking to the
     * broker itself and hands every MQTT message to the attached client as a
     * MqttClientProxyMessage for it to relay - and this client ignores that FromRadio variant
     * entirely. Offering the toggle would let the Brick silently take the radio's MQTT off
     * the air; showing the setting still tells you why MQTT is not working if a phone left it
     * on. Editable once we speak the proxy protocol, not before.
     */
    item_toggle(list, MESH_STR_SETTINGS_PROXY_VIA_CLIENT, s->mqtt_proxy_to_client_enabled);
}

static void build_store_forward(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_SF_ENABLED, s->store_forward_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SF_HEARTBEAT, s->store_forward_heartbeat ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SF_SERVER, s->store_forward_is_server ? 1U : 0U, NULL);
    /* The three the radio only reads as a server. Shown regardless: a node is set up to be a
       server by filling these in and then turning the row above on. */
    item_heading(list, MESH_STR_HEAD_SERVER);
    item_field(list, MESH_UI_FIELD_SF_RECORDS, s->store_forward_records, NULL);
    item_field(list, MESH_UI_FIELD_SF_HISTORY_MAX, s->store_forward_history_return_max, NULL);
    item_field(list, MESH_UI_FIELD_SF_HISTORY_WINDOW, s->store_forward_history_return_window, NULL);
}

/*
 * Telemetry is five near-identical groups - a toggle, an interval, sometimes a screen flag -
 * and fifteen fields of that in a flat run is unreadable. The headings are what phase 9 added
 * them for, and they are why the rows inside a group are named "Enabled" and "Interval"
 * rather than repeating the group in every label.
 */
static void build_telemetry(const struct mesh_ui_settings *s, struct item_list *list) {
    item_heading(list, MESH_STR_HEAD_DEVICE);
    item_field(list, MESH_UI_FIELD_TELEMETRY_DEVICE, s->device_telemetry_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_INTERVAL, s->device_update_interval, NULL);
    item_heading(list, MESH_STR_HEAD_ENVIRONMENT);
    item_field(list, MESH_UI_FIELD_TELEMETRY_ENVIRONMENT,
               s->environment_measurement_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_ENV_INTERVAL, s->environment_update_interval, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_ENV_SCREEN, s->environment_screen_enabled ? 1U : 0U,
               NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_ENV_FAHRENHEIT,
               s->environment_display_fahrenheit ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_AIR_QUALITY);
    item_field(list, MESH_UI_FIELD_TELEMETRY_AIR_QUALITY, s->air_quality_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_AIR_INTERVAL, s->air_quality_interval, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_AIR_SCREEN, s->air_quality_screen_enabled ? 1U : 0U,
               NULL);
    item_heading(list, MESH_STR_HEAD_POWER);
    item_field(list, MESH_UI_FIELD_TELEMETRY_POWER, s->power_measurement_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_POWER_INTERVAL, s->power_update_interval, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_POWER_SCREEN, s->power_screen_enabled ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_HEALTH);
    item_field(list, MESH_UI_FIELD_TELEMETRY_HEALTH, s->health_measurement_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_HEALTH_INTERVAL, s->health_update_interval, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_HEALTH_SCREEN, s->health_screen_enabled ? 1U : 0U,
               NULL);
}

static void build_neighbor_info(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_NEIGHBOR_ENABLED, s->neighbor_info_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_NEIGHBOR_INTERVAL, s->neighbor_info_interval, NULL);
    item_field(list, MESH_UI_FIELD_NEIGHBOR_OVER_LORA, s->neighbor_info_over_lora ? 1U : 0U, NULL);
}

/*
 * Range test. The module is two different things depending on one row: with `sender` at 0 it
 * only listens, and with anything else this node transmits to the whole channel on that timer.
 * The heading says so, because a row reading "Send every  30s" does not convey that the traffic
 * lands on everyone else's radio too.
 */
static void build_range_test(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_RANGE_TEST_ENABLED, s->range_test_enabled ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_TRANSMITTER);
    item_str(list, MESH_STR_NOTE_TEST_PACKETS, MESH_UI_SETTING_INFO,
             MESH_STR_NOTE_TEST_PACKETS_VALUE);
    item_field(list, MESH_UI_FIELD_RANGE_TEST_SENDER, s->range_test_sender, NULL);
    /* ESP32-only in the firmware; shown anyway, because the radio ignoring a flag is quieter
       than the row not being there when a phone app shows it. */
    item_heading(list, MESH_STR_HEAD_LOG_ESP32);
    item_field(list, MESH_UI_FIELD_RANGE_TEST_SAVE, s->range_test_save ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_RANGE_TEST_CLEAR, s->range_test_clear_on_reboot ? 1U : 0U, NULL);
}

static void build_paxcounter(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_PAX_ENABLED, s->paxcounter_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_PAX_INTERVAL, s->paxcounter_interval, NULL);
    /* Signed on the wire and signed in the store; the cast is the row model's, not the value's
       (see k_rssi_presets). A radio that has never had these set reports 0, which is not a
       threshold the module uses - the firmware substitutes -80. */
    item_heading(list, MESH_STR_HEAD_COUNT_ABOVE);
    item_field(list, MESH_UI_FIELD_PAX_WIFI_THRESHOLD,
               (uint32_t)(s->paxcounter_wifi_threshold != 0 ? s->paxcounter_wifi_threshold : -80),
               NULL);
    item_field(list, MESH_UI_FIELD_PAX_BLE_THRESHOLD,
               (uint32_t)(s->paxcounter_ble_threshold != 0 ? s->paxcounter_ble_threshold : -80),
               NULL);
}

static void build_tak(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_TAK_TEAM, s->tak_team, NULL);
    item_field(list, MESH_UI_FIELD_TAK_ROLE, s->tak_role, NULL);
}

static void build_ambient(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_AMBIENT_LED, s->ambient_led_state ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_AMBIENT_CURRENT, s->ambient_current, NULL);
    /* Three channels rather than a colour picker: a d-pad steps numbers well and picks colours
       badly. Listed under a heading so the trio reads as one setting. */
    item_heading(list, MESH_STR_HEAD_COLOUR);
    item_field(list, MESH_UI_FIELD_AMBIENT_RED, s->ambient_red, NULL);
    item_field(list, MESH_UI_FIELD_AMBIENT_GREEN, s->ambient_green, NULL);
    item_field(list, MESH_UI_FIELD_AMBIENT_BLUE, s->ambient_blue, NULL);
}

static void build_status_message(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_STATUS_TEXT, 0U, s->status_message);
}

static void build_detection(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_DETECT_ENABLED, s->detection_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_DETECT_NAME, 0U, s->detection_name);
    item_field(list, MESH_UI_FIELD_DETECT_MIN_BROADCAST, s->detection_minimum_broadcast_secs, NULL);
    item_field(list, MESH_UI_FIELD_DETECT_STATE_BROADCAST, s->detection_state_broadcast_secs, NULL);
    item_field(list, MESH_UI_FIELD_DETECT_SEND_BELL, s->detection_send_bell ? 1U : 0U, NULL);
    /* The pin and how it is read. Which pins a board exposes is its own business and nothing
       on the wire says, so the rows are offered and the heading says whose problem it is. */
    item_heading(list, MESH_STR_HEAD_WIRING);
    item_field(list, MESH_UI_FIELD_DETECT_PIN, s->detection_monitor_pin, NULL);
    item_field(list, MESH_UI_FIELD_DETECT_TRIGGER, s->detection_trigger_type, NULL);
    item_field(list, MESH_UI_FIELD_DETECT_PULLUP, s->detection_use_pullup ? 1U : 0U, NULL);
}

/*
 * External notification: fifteen fields, of which nine are three near-identical groups - a
 * pin, an alert on a message, an alert on a bell, once each for the plain output, the vibra
 * motor and the buzzer. Flat, that is nine rows whose labels differ by one word and which
 * cannot be told apart at a glance; under three headings it is three copies of the same
 * three-row shape. This is the module the heading row was added for.
 */
static void build_ext_notification(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ENABLED, s->extnotif_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_OUTPUT_MS, s->extnotif_output_ms, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_NAG, s->extnotif_nag_timeout, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ACTIVE, s->extnotif_active ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_PWM, s->extnotif_use_pwm ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_I2S, s->extnotif_use_i2s_as_buzzer ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_OUTPUT);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_PIN, s->extnotif_output, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_MSG, s->extnotif_alert_message ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_BELL, s->extnotif_alert_bell ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_VIBRA);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_PIN_VIBRA, s->extnotif_output_vibra, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_VIBRA,
               s->extnotif_alert_message_vibra ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_VIBRA,
               s->extnotif_alert_bell_vibra ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_BUZZER);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_PIN_BUZZER, s->extnotif_output_buzzer, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_BUZZER,
               s->extnotif_alert_message_buzzer ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_BUZZER,
               s->extnotif_alert_bell_buzzer ? 1U : 0U, NULL);
}

/*
 * Traffic management is the one module with no enabled toggle: upstream removed the bool
 * flags in favour of "a non-zero value implicitly enables it", and reserved their tags so
 * nobody puts them back. Every row here is therefore off at 0, and the section says so rather
 * than leaving a reader to wonder where the switch is.
 */
static void build_traffic(const struct mesh_ui_settings *s, struct item_list *list) {
    item_str(list, MESH_STR_NOTE_EACH_LIMIT, MESH_UI_SETTING_INFO, MESH_STR_NOTE_EACH_LIMIT_VALUE);
    item_field(list, MESH_UI_FIELD_TRAFFIC_POSITION_INTERVAL, s->traffic_position_min_interval_secs,
               NULL);
    item_field(list, MESH_UI_FIELD_TRAFFIC_NODEINFO_HOPS, s->traffic_nodeinfo_max_hops, NULL);
    item_heading(list, MESH_STR_HEAD_RATE_LIMIT);
    item_field(list, MESH_UI_FIELD_TRAFFIC_RATE_WINDOW, s->traffic_rate_limit_window_secs, NULL);
    item_field(list, MESH_UI_FIELD_TRAFFIC_RATE_PACKETS, s->traffic_rate_limit_max_packets, NULL);
    item_field(list, MESH_UI_FIELD_TRAFFIC_UNKNOWN_THRESHOLD, s->traffic_unknown_packet_threshold,
               NULL);
}

/*
 * Radio actions: the rows that make the radio do something rather than keep something. None of
 * them is a setting, so there is no Y to press and nothing to read back - A on a row opens the
 * confirm overlay and the answer goes out on its own.
 *
 * Ordered least to most destructive, so a cursor arriving at the top of the list is on the one
 * press here that costs nothing but a reconnect, and the two that cannot be undone are the
 * furthest to travel to.
 *
 * The two forget rows are the exception to the section's own name: they drop this client's
 * cached roster and send nothing. They live here because a NodeDB reset leaves that roster
 * standing on purpose - the radio holds 80 entries and evicts, so ours is often the only copy
 * - and this is the screen somebody who has just reset the radio is looking at while wondering
 * why the Nodes tab still says 81. Each says how many it would drop, so the press is not a
 * guess, and a row with nothing to drop is an INFO row rather than a press that does nothing.
 *
 * Which is what the four headings are for. Seven bare verbs in a column give no clue that one
 * of them empties the radio's database and the next two empty ours - the distinction that
 * makes the Status screen say 2 nodes while the Nodes tab says 81. "Nodes on the radio" and
 * "Nodes cached here" say whose is whose before the press rather than in the confirm text
 * after it, and they read as a pair because the rows under them are one.
 *
 * The labels stop at the noun the value column supplies - the label column is 20 cells and
 * "Forget off-radio nodes" is 22 - so the row reads across as one sentence rather than as a
 * clipped one: "Forget off-radio > 7 nodes".
 */
static void build_actions(const struct mesh_ui_settings *s,
                          const struct mesh_ui_handshake_state *handshake, struct item_list *list) {
    /* An AdminMessage cannot be addressed without our own node number, so with no link every
       row here but the two forget rows is unpressable. They say so rather than disappearing:
       a section whose length changes when the radio drops moves the cursor out from under the
       user, and "not connected" is the answer they were about to press A to find out. */
    const bool connected = handshake != NULL && handshake->has_my_info;

    item_heading(list, MESH_STR_HEAD_POWER);
    item_radio_action(list, MESH_STR_ACTION_REBOOT, MESH_UI_SETTINGS_ACTION_REBOOT, connected);
    /* DeviceMetadata says whether the hardware can cut its own power; on a board that cannot,
       the request is simply ignored, so the row says so rather than lying about what A does.
       Until the metadata arrives the row is offered: the radio is the authority, not us. */
    if (s->has_metadata && !s->can_shutdown) {
        item_str(list, MESH_STR_ACTION_SHUTDOWN, MESH_UI_SETTING_INFO,
                 MESH_STR_ACTION_SHUTDOWN_UNSUPPORTED);
    } else {
        item_radio_action(list, MESH_STR_ACTION_SHUTDOWN, MESH_UI_SETTINGS_ACTION_SHUTDOWN,
                          connected);
    }
    item_heading(list, MESH_STR_HEAD_NODES_RADIO);
    item_radio_action(list, MESH_STR_ACTION_RESET_NODEDB, MESH_UI_SETTINGS_ACTION_RESET_NODEDB,
                      connected);

    /* Both numbers are what the press would remove, not what is cached or stale: a forget
       keeps our own record and every pin, so a roster of eighty nodes that are all pinned has
       nothing to drop and both rows say so. */
    item_heading(list, MESH_STR_HEAD_NODES_CACHED);
    forget_row(list, MESH_STR_ACTION_FORGET_OFF_RADIO,
               handshake != NULL ? handshake->nodes_forgettable_off_radio : 0U,
               MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES);
    forget_row(list, MESH_STR_ACTION_FORGET_ALL,
               handshake != NULL ? handshake->nodes_forgettable_all : 0U,
               MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES);

    item_heading(list, MESH_STR_HEAD_FACTORY_RESET);
    item_radio_action(list, MESH_STR_ACTION_FACTORY_CONFIG,
                      MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG, connected);
    item_radio_action(list, MESH_STR_ACTION_FACTORY_DEVICE,
                      MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE, connected);
}

static void build_section(const struct mesh_ui_settings *settings,
                          const struct mesh_ui_handshake_state *handshake,
                          const struct mesh_ui_setting_edit *edits, size_t edit_count,
                          enum mesh_ui_settings_section section, uint8_t channel,
                          struct item_list *list) {
    memset(list, 0, sizeof *list);
    list->edits = edits;
    list->edit_count = edits != NULL ? edit_count : 0U;
    if (settings == NULL || !mesh_ui_settings_section_loaded(settings, handshake, section)) {
        return;
    }
    switch (section) {
    case MESH_UI_SETTINGS_ABOUT:
        build_about(settings, list);
        break;
    case MESH_UI_SETTINGS_RADIO:
        build_radio(settings, handshake, list);
        break;
    case MESH_UI_SETTINGS_USER:
        build_user(settings, list);
        break;
    case MESH_UI_SETTINGS_DEVICE:
        build_device(settings, list);
        break;
    case MESH_UI_SETTINGS_DISPLAY:
        build_display(settings, list);
        break;
    case MESH_UI_SETTINGS_LORA:
        build_lora(settings, list);
        break;
    case MESH_UI_SETTINGS_BLUETOOTH:
        build_bluetooth(settings, list);
        break;
    case MESH_UI_SETTINGS_CHANNELS:
        if (channel != MESH_UI_SETTINGS_NO_CHANNEL) {
            build_channel(settings, channel, list);
        } else {
            build_channels(settings, handshake, list);
        }
        break;
    case MESH_UI_SETTINGS_SECURITY:
        build_security(settings, list);
        break;
    case MESH_UI_SETTINGS_POSITION:
        build_position(settings, list);
        break;
    case MESH_UI_SETTINGS_POWER:
        build_power(settings, list);
        break;
    case MESH_UI_SETTINGS_MQTT:
        build_mqtt(settings, list);
        break;
    case MESH_UI_SETTINGS_STORE_FORWARD:
        build_store_forward(settings, list);
        break;
    case MESH_UI_SETTINGS_TELEMETRY:
        build_telemetry(settings, list);
        break;
    case MESH_UI_SETTINGS_ACTIONS:
        build_actions(settings, handshake, list);
        break;
    case MESH_UI_SETTINGS_MODULES:
        build_modules(settings, handshake, list);
        break;
    case MESH_UI_SETTINGS_NEIGHBOR_INFO:
        build_neighbor_info(settings, list);
        break;
    case MESH_UI_SETTINGS_RANGE_TEST:
        build_range_test(settings, list);
        break;
    case MESH_UI_SETTINGS_PAXCOUNTER:
        build_paxcounter(settings, list);
        break;
    case MESH_UI_SETTINGS_TAK:
        build_tak(settings, list);
        break;
    case MESH_UI_SETTINGS_AMBIENT:
        build_ambient(settings, list);
        break;
    case MESH_UI_SETTINGS_STATUS_MESSAGE:
        build_status_message(settings, list);
        break;
    case MESH_UI_SETTINGS_DETECTION:
        build_detection(settings, list);
        break;
    case MESH_UI_SETTINGS_EXT_NOTIFICATION:
        build_ext_notification(settings, list);
        break;
    case MESH_UI_SETTINGS_TRAFFIC:
        build_traffic(settings, list);
        break;
    default:
        break;
    }
}

uint32_t mesh_ui_settings_item_count(const struct mesh_ui_settings *settings,
                                     const struct mesh_ui_handshake_state *handshake,
                                     enum mesh_ui_settings_section section, uint8_t channel) {
    struct item_list list;
    build_section(settings, handshake, NULL, 0U, section, channel, &list);
    return list.count;
}

bool mesh_ui_settings_item(const struct mesh_ui_settings *settings,
                           const struct mesh_ui_handshake_state *handshake,
                           const struct mesh_ui_setting_edit *edits, size_t edit_count,
                           enum mesh_ui_settings_section section, uint8_t channel, uint32_t row,
                           struct mesh_ui_settings_item *out) {
    if (out == NULL) {
        return false;
    }
    struct item_list list;
    build_section(settings, handshake, edits, edit_count, section, channel, &list);
    if (row >= list.count) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = list.items[row];
    return true;
}
