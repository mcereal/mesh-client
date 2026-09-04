#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_store;
struct mesh_ui_message_list;

/*
 * Logical buttons. Backends and the input layer translate from whatever the hardware reports
 * (evdev codes on the Brick, keyboard keys on a host) so the navigation model never sees a
 * keycode. The names follow the Brick's face buttons: A confirms, B backs out.
 */
enum mesh_ui_key {
    MESH_UI_KEY_NONE = 0,
    MESH_UI_KEY_UP,
    MESH_UI_KEY_DOWN,
    MESH_UI_KEY_LEFT,
    MESH_UI_KEY_RIGHT,
    MESH_UI_KEY_A,
    MESH_UI_KEY_B,
    MESH_UI_KEY_X,
    MESH_UI_KEY_Y,
    MESH_UI_KEY_L1,
    MESH_UI_KEY_R1,
    MESH_UI_KEY_START,
    MESH_UI_KEY_SELECT,
};

/* Tabs, in the order LEFT/RIGHT (and L1/R1) walk them. */
enum mesh_ui_screen {
    MESH_UI_SCREEN_MESSAGES = 0,
    MESH_UI_SCREEN_NODES,
    MESH_UI_SCREEN_COMPOSE,
    MESH_UI_SCREEN_DEVICES,
    MESH_UI_SCREEN_STATUS,
    MESH_UI_SCREEN_SETTINGS,
    MESH_UI_SCREEN_COUNT,
};

#define MESH_UI_NAV_TARGET_NAME_MAX 40U
/* nav.settings_section when the Settings tab shows the section list rather than a section. */
#define MESH_UI_SETTINGS_NO_SECTION 0xFFU
#define MESH_UI_NAV_TOAST_MAX 64U
#define MESH_UI_CANNED_MAX 16U
#define MESH_UI_CANNED_TEXT_MAX 64U
/* Upstream Data.payload caps at 233 bytes; the draft and action text hold that plus a NUL. */
#define MESH_UI_DRAFT_MAX 234U
/* Pending Settings edits held until Save, and the longest text a setting can take. */
#define MESH_UI_SETTINGS_EDITS_MAX 8U
#define MESH_UI_SETTING_TEXT_MAX 40U

/* One edited setting. `field` is an enum mesh_ui_setting_field (settings.h); NONE marks an
   empty slot. Toggles and enums use `number`, numbers use `number`, text uses `text`. */
struct mesh_ui_setting_edit {
    uint8_t field;
    uint32_t number;
    char text[MESH_UI_SETTING_TEXT_MAX];
};

/* On-screen keyboard geometry: four rows of ten characters and a row of five actions. */
#define MESH_UI_KB_COLS 10U
#define MESH_UI_KB_CHAR_ROWS 4U
#define MESH_UI_KB_ROWS (MESH_UI_KB_CHAR_ROWS + 1U)
#define MESH_UI_KB_ACTIONS 5U

enum mesh_ui_kb_layer {
    MESH_UI_KB_LOWER = 0,
    MESH_UI_KB_UPPER,
    MESH_UI_KB_SYMBOLS,
    MESH_UI_KB_LAYER_COUNT,
};

enum mesh_ui_kb_action {
    MESH_UI_KB_ACTION_LAYER = 0, /* cycle lower/upper/symbols */
    MESH_UI_KB_ACTION_SPACE,
    MESH_UI_KB_ACTION_DELETE,
    MESH_UI_KB_ACTION_SEND,
    MESH_UI_KB_ACTION_CANCEL,
};

/*
 * Everything a backend needs to draw a cursor, the compose target and the keyboard. Lives in
 * the store and is copied into each snapshot, so backends stay stateless.
 *
 * The conversation model: `target_node` is either MESH_MESSAGE_BROADCAST_ADDR (the channel
 * `target_channel`) or a node number. The Messages tab shows that conversation, or everything
 * when `inbox` is set. Compose sends to it. The Nodes tab, a message, and the Compose To: row
 * all set it.
 */
struct mesh_ui_nav {
    enum mesh_ui_screen screen;
    uint32_t cursor[MESH_UI_SCREEN_COUNT];
    uint32_t target_node;
    uint8_t target_channel;
    bool inbox;
    char target_name[MESH_UI_NAV_TARGET_NAME_MAX];
    /* One-line transient notice ("Sent to ABCD", "Connecting..."); empty when none. */
    char toast[MESH_UI_NAV_TOAST_MAX];
    uint64_t toast_until_ms;
    /* Filtered message count at the last clamp, so a cursor parked on the newest message
       follows new traffic instead of being left behind. */
    uint32_t messages_seen;
    /* "Send to" picker over the Compose tab: every enabled channel, then every node. */
    bool picker_open;
    uint32_t picker_cursor;
    /* Free-text entry. */
    bool keyboard_open;
    uint8_t kb_row;
    uint8_t kb_col;
    uint8_t kb_layer; /* enum mesh_ui_kb_layer */
    char draft[MESH_UI_DRAFT_MAX];
    /* Settings tab: the open section (enum mesh_ui_settings_section) or NO_SECTION for the
       section list. cursor[SETTINGS] indexes whichever list is showing; the section list's
       position is parked here while a section is open. */
    uint8_t settings_section;
    uint32_t settings_list_cursor;
    /* Edits made in the open section and not yet saved. Y sends them as one
       MESH_UI_ACTION_SAVE_SETTINGS; B asks once (discard_armed) and discards on the second
       press. The app clears them through mesh_ui_store_settings_edits_clear() once queued. */
    struct mesh_ui_setting_edit settings_edits[MESH_UI_SETTINGS_EDITS_MAX];
    uint8_t settings_edit_count;
    bool settings_discard_armed;
    /* When the keyboard edits a setting rather than the Compose draft: the field it is for
       (NONE for Compose) and the Compose draft parked while it is open. */
    uint8_t keyboard_field;
    char draft_saved[MESH_UI_DRAFT_MAX];
};

enum mesh_ui_action_type {
    MESH_UI_ACTION_NONE = 0,
    MESH_UI_ACTION_CONNECT,          /* identifier = BLE address */
    MESH_UI_ACTION_SEND_TEXT,        /* dest/channel/text */
    MESH_UI_ACTION_REFRESH_SETTINGS, /* re-read the radio's configuration */
    MESH_UI_ACTION_SAVE_SETTINGS,    /* section + edits: write one section to the radio */
};

struct mesh_ui_action {
    enum mesh_ui_action_type type;
    char identifier[64];
    uint32_t dest;
    uint8_t channel;
    char text[MESH_UI_DRAFT_MAX];
    /* SAVE_SETTINGS: the section (enum mesh_ui_settings_section) and its pending edits. */
    uint8_t section;
    uint8_t edit_count;
    struct mesh_ui_setting_edit edits[MESH_UI_SETTINGS_EDITS_MAX];
};

void mesh_ui_nav_init(struct mesh_ui_nav *nav);

/* Applies one button press. Returns true when the visible state changed. When the press
   asks the app to do something, *out_action is filled in (may be NULL to discard). The store
   is read for list sizes and to resolve node names; it is not modified. */
bool mesh_ui_nav_handle_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                            enum mesh_ui_key key, struct mesh_ui_action *out_action);

/* Keeps cursors inside their lists after the data changed. Returns true if anything moved. */
bool mesh_ui_nav_clamp(struct mesh_ui_nav *nav, const struct mesh_ui_store *store);

/* Rows on a screen for the current data (the Compose tab counts its To: and draft rows; the
   Messages tab counts only the current conversation). */
uint32_t mesh_ui_nav_row_count(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                               enum mesh_ui_screen screen);

/* Indices into `messages` that belong to the conversation the nav is showing, oldest first.
   Returns how many were written (at most `capacity`). */
uint32_t mesh_ui_nav_filter_messages(const struct mesh_ui_nav *nav,
                                     const struct mesh_ui_message_list *messages,
                                     uint32_t *out_indices, uint32_t capacity);

/* Human name for the current conversation: "Inbox", "#LongFast", "BRVO". */
void mesh_ui_nav_conversation_name(const struct mesh_ui_nav *nav, char *out, size_t out_len);

/* The picker's rows: channels first (node_id = MESH_MESSAGE_BROADCAST_ADDR, channel set), then
   nodes other than ourselves. Returns the row count; mesh_ui_nav_picker_row() describes one. */
uint32_t mesh_ui_nav_picker_count(const struct mesh_ui_store *store);
bool mesh_ui_nav_picker_row(const struct mesh_ui_store *store, uint32_t index, uint32_t *out_node,
                            uint8_t *out_channel, char *out_name, size_t out_name_len);

/* Compose rows: 0 = To:, 1 = draft, then the canned replies. */
#define MESH_UI_COMPOSE_ROW_TARGET 0U
#define MESH_UI_COMPOSE_ROW_DRAFT 1U
#define MESH_UI_COMPOSE_FIRST_CANNED 2U

/* Keyboard legend for the backends. Character rows return the glyph at that cell (a NUL for an
   unused cell); the action row is described by mesh_ui_kb_action_label(). */
char mesh_ui_kb_char(enum mesh_ui_kb_layer layer, unsigned row, unsigned col);
const char *mesh_ui_kb_action_label(const struct mesh_ui_nav *nav, enum mesh_ui_kb_action action);

void mesh_ui_nav_set_toast(struct mesh_ui_nav *nav, uint64_t now_ms, const char *text);
/* Clears an expired toast; returns true if it did. */
bool mesh_ui_nav_tick(struct mesh_ui_nav *nav, uint64_t now_ms);

const char *mesh_ui_screen_name(enum mesh_ui_screen screen);

/* Canned replies shown on the Compose tab. Defaults are built in; a file with one message per
   line (blank lines and '#' comments skipped) replaces them. */
size_t mesh_ui_canned_count(void);
const char *mesh_ui_canned_text(size_t index);
int mesh_ui_canned_load(const char *path);
void mesh_ui_canned_reset(void);

#ifdef __cplusplus
}
#endif
