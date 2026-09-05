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

/* Tabs, in the order LEFT/RIGHT (and L1/R1) walk them. Compose is not one: it is an overlay
   over the open conversation, so it can never be reached with a stale destination. */
enum mesh_ui_screen {
    MESH_UI_SCREEN_MESSAGES = 0,
    MESH_UI_SCREEN_NODES,
    MESH_UI_SCREEN_DEVICES,
    MESH_UI_SCREEN_STATUS,
    MESH_UI_SCREEN_SETTINGS,
    MESH_UI_SCREEN_COUNT,
};

#define MESH_UI_NAV_TARGET_NAME_MAX 40U
/* nav.settings_section when the Settings tab shows the section list rather than a section. */
#define MESH_UI_SETTINGS_NO_SECTION 0xFFU
/* nav.settings_channel when the Channels section shows its list rather than one channel. */
#define MESH_UI_SETTINGS_NO_CHANNEL 0xFFU
#define MESH_UI_NAV_TOAST_MAX 64U
#define MESH_UI_CANNED_MAX 16U
#define MESH_UI_CANNED_TEXT_MAX 64U
/* Upstream Data.payload caps at 233 bytes; the draft and action text hold that plus a NUL. */
#define MESH_UI_DRAFT_MAX 234U
/* Pending Settings edits held until Save, and the longest text a setting can take. */
#define MESH_UI_SETTINGS_EDITS_MAX 8U
/* Long enough for a 32-byte key typed as hex. */
#define MESH_UI_SETTING_TEXT_MAX 72U

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
 * The conversation model, in the shape the Settings tab already uses: the Messages tab is two
 * levels. With `thread_open` clear it lists conversations (all traffic, each enabled channel,
 * each node we have direct messages with, then "New message"); with it set it shows the one
 * conversation named by `target_node` (MESH_MESSAGE_BROADCAST_ADDR means the channel
 * `target_channel`) or, when `inbox` is set, every message. Compose is an overlay over the open
 * thread and always sends there.
 *
 * The invariant that keeps this predictable: *only* opening a thread moves the target. The
 * Nodes tab opens the node's thread rather than retargeting whatever Messages was showing.
 */
struct mesh_ui_nav {
    enum mesh_ui_screen screen;
    uint32_t cursor[MESH_UI_SCREEN_COUNT];
    uint32_t target_node;
    uint8_t target_channel;
    /* Messages tab: a thread is open (cursor[MESSAGES] indexes its messages) rather than the
       conversation list, whose position is parked in conversation_list_cursor meanwhile. */
    bool thread_open;
    uint32_t conversation_list_cursor;
    /* The open thread is the all-traffic one; meaningless unless thread_open. */
    bool inbox;
    char target_name[MESH_UI_NAV_TARGET_NAME_MAX];
    /* One-line transient notice ("Sent to ABCD", "Connecting..."); empty when none. */
    char toast[MESH_UI_NAV_TOAST_MAX];
    uint64_t toast_until_ms;
    /* Filtered message count at the last clamp, so a cursor parked on the newest message
       follows new traffic instead of being left behind. */
    uint32_t messages_seen;
    /* Compose overlay over the open thread: the draft row, then the canned replies. */
    bool compose_open;
    uint32_t compose_cursor;
    /* "Send to" picker: every enabled channel, then every node. Picking opens that
       conversation's thread, and when `picker_to_compose` is set (the "New message" row) the
       compose overlay with it. */
    bool picker_open;
    uint32_t picker_cursor;
    bool picker_to_compose;
    /* Free-text entry. */
    bool keyboard_open;
    uint8_t kb_row;
    uint8_t kb_col;
    uint8_t kb_layer; /* enum mesh_ui_kb_layer */
    char draft[MESH_UI_DRAFT_MAX];
    /* Nodes tab: a node's detail is open (cursor[NODES] indexes its rows) rather than the node
       list, whose position is parked in node_list_cursor meanwhile. The same two-level shape
       as Settings and Messages. Opening a detail does *not* move the compose target; only the
       detail's "Message this node" row does. */
    bool node_detail_open;
    uint32_t node_detail_node; /* the open node's id: the list is re-ranked under us */
    uint32_t node_list_cursor;
    /* Settings tab: the open section (enum mesh_ui_settings_section) or NO_SECTION for the
       section list. cursor[SETTINGS] indexes whichever list is showing; the section list's
       position is parked here while a section is open. */
    uint8_t settings_section;
    uint32_t settings_list_cursor;
    /* Channels section: the open channel slot or NO_CHANNEL for the channel list, whose
       position is parked in settings_channel_list_cursor while a channel is open. */
    uint8_t settings_channel;
    uint32_t settings_channel_list_cursor;
    /* "Save <section>?" overlay for sections whose write can cut this client off (Bluetooth,
       Channels). Row 0 saves, row 1 cancels. */
    bool confirm_open;
    uint8_t confirm_cursor;
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
    /* BLE pairing prompt. The keyboard is retargeted for it the same way a setting's text
       retargets it, except that this one is opened by the app rather than by a key press:
       BlueZ asks for the PIN somewhere in the middle of a connect and blocks until it is
       answered. `pairing_confirm` marks the numeric-comparison case, where the digits are
       pre-filled and Send means "yes, that is what the node is showing". */
    bool keyboard_passkey;
    bool pairing_confirm;
    char pairing_label[MESH_UI_NAV_TARGET_NAME_MAX];
    /* The keyboard target the prompt displaced, restored when it closes. The prompt can land
       on top of an open keyboard, and the text being typed is parked in `draft_saved` like any
       other. (A settings keyboard that had itself parked a compose draft loses that one: there
       is a single parking slot, and the text in front of the user is the one worth keeping.) */
    uint8_t keyboard_field_displaced;
    /* Devices tab: Y is armed by one press and forgets the node on the second, because a
       bond dropped by accident costs the user a re-pair with the PIN. */
    bool devices_forget_armed;
    uint32_t devices_forget_row;
};

enum mesh_ui_action_type {
    MESH_UI_ACTION_NONE = 0,
    MESH_UI_ACTION_CONNECT,          /* identifier = BLE address */
    MESH_UI_ACTION_SEND_TEXT,        /* dest/channel/text */
    MESH_UI_ACTION_REFRESH_SETTINGS, /* re-read the radio's configuration */
    MESH_UI_ACTION_SAVE_SETTINGS,    /* section + edits: write one section to the radio */
    MESH_UI_ACTION_TOGGLE_FAVORITE,  /* dest = node to pin/unpin; `number` is 1 to pin */
    /* About section: ask GitHub what the newest release is, and install the one a check
       found. Two actions rather than one because installing replaces the running binary. */
    MESH_UI_ACTION_CHECK_UPDATE,
    MESH_UI_ACTION_INSTALL_UPDATE,
    /* Devices tab. DISCONNECT with an empty identifier means "whatever link is up": only one
       radio is ever connected, so the row the cursor happens to be on does not decide it. */
    MESH_UI_ACTION_DISCONNECT,
    MESH_UI_ACTION_FORGET, /* identifier = BLE address to unpair */
    /* Answers the BlueZ pairing agent: `text` holds the digits typed into the prompt. */
    MESH_UI_ACTION_SUBMIT_PASSKEY,
    MESH_UI_ACTION_CANCEL_PAIRING,
};

struct mesh_ui_action {
    enum mesh_ui_action_type type;
    char identifier[64];
    /* CONNECT: which transport the picked row belongs to (enum mesh_ui_device_kind). The
       Devices tab lists BLE advertisers and USB ports together, so the app cannot infer it
       from the identifier. */
    uint8_t kind;
    uint32_t dest;
    uint8_t channel;
    /* TOGGLE_FAVORITE: 1 to pin, 0 to unpin. The nav reads the node's current flag and sends
       the state it wants, so a press that races a NodeInfo cannot end up as a no-op toggle. */
    uint32_t number;
    char text[MESH_UI_DRAFT_MAX];
    /* SAVE_SETTINGS: the section (enum mesh_ui_settings_section), the channel slot for the
       Channels section (in `channel`), and the pending edits. */
    uint8_t section;
    uint8_t edit_count;
    struct mesh_ui_setting_edit edits[MESH_UI_SETTINGS_EDITS_MAX];
};

void mesh_ui_nav_init(struct mesh_ui_nav *nav);

/* The PIN a passkey prompt accepts: BlueZ passkeys are 0-999999. */
#define MESH_UI_PASSKEY_DIGITS 6U

/* Opens (or closes) the PIN prompt over whatever the user was doing. Driven by the app from
   the pairing agent, not by a key press, so it lives outside mesh_ui_nav_handle_key(). For a
   numeric comparison (`confirm`) the digits are pre-filled and Send accepts them. Returns
   true when the frame needs repainting. */
bool mesh_ui_nav_open_passkey(struct mesh_ui_nav *nav, const char *label, uint32_t passkey,
                              bool confirm);
bool mesh_ui_nav_close_passkey(struct mesh_ui_nav *nav);

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

/* Human name for the open thread: "All traffic", "#LongFast", "BRVO", or "Messages" when the
   conversation list is showing. */
void mesh_ui_nav_conversation_name(const struct mesh_ui_nav *nav, char *out, size_t out_len);

/* What a row of the Messages tab's conversation list is. */
enum mesh_ui_conversation_kind {
    MESH_UI_CONVERSATION_ALL = 0, /* every message, whatever it belongs to */
    MESH_UI_CONVERSATION_CHANNEL,
    MESH_UI_CONVERSATION_DIRECT,
    MESH_UI_CONVERSATION_NEW, /* the "New message" row: opens the send-to picker */
};

#define MESH_UI_CONVERSATION_PREVIEW_MAX 96U

struct mesh_ui_conversation {
    uint8_t kind; /* enum mesh_ui_conversation_kind */
    /* Where a message to this conversation goes, in the same terms as nav.target_*. */
    uint32_t node;
    uint8_t channel;
    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    /* Newest message in the conversation, for the list's second line. Empty and zero when the
       conversation has no traffic yet. */
    char preview[MESH_UI_CONVERSATION_PREVIEW_MAX];
    uint32_t last_time;
    uint32_t message_count;
    bool preview_outbound;
    /* Inbound messages that arrived after this conversation was last read. The "All traffic"
       row carries the total across every other row rather than a mark of its own. */
    uint32_t unread;
};

/*
 * The conversation list: "All traffic", then every enabled channel in the radio's table order,
 * then every node we have direct messages with newest first, then "New message". Channels keep
 * the radio's order rather than sorting by recency so a row does not move out from under the
 * cursor while the user is reaching for it.
 */
uint32_t mesh_ui_nav_conversation_count(const struct mesh_ui_store *store);
bool mesh_ui_nav_conversation_at(const struct mesh_ui_store *store, uint32_t index,
                                 struct mesh_ui_conversation *out);
/* True when the nav's open thread is the conversation on that row. */
bool mesh_ui_nav_conversation_is_open(const struct mesh_ui_nav *nav,
                                      const struct mesh_ui_conversation *conversation);

/* Inbound messages across every channel and peer that have not been read. */
uint32_t mesh_ui_nav_unread_total(const struct mesh_ui_store *store);

/* The picker's rows: channels first (node_id = MESH_MESSAGE_BROADCAST_ADDR, channel set), then
   nodes other than ourselves. Returns the row count; mesh_ui_nav_picker_row() describes one. */
uint32_t mesh_ui_nav_picker_count(const struct mesh_ui_store *store);
bool mesh_ui_nav_picker_row(const struct mesh_ui_store *store, uint32_t index, uint32_t *out_node,
                            uint8_t *out_channel, char *out_name, size_t out_name_len);

/* Compose overlay rows: 0 = the draft, then the canned replies. There is no To: row; the
   overlay only ever opens over a thread, and that thread is the destination. */
#define MESH_UI_COMPOSE_ROW_DRAFT 0U
#define MESH_UI_COMPOSE_FIRST_CANNED 1U
uint32_t mesh_ui_nav_compose_row_count(void);

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
