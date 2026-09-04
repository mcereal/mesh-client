#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_store;

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
    MESH_UI_SCREEN_COUNT,
};

#define MESH_UI_NAV_TARGET_NAME_MAX 40U
#define MESH_UI_NAV_TOAST_MAX 64U
#define MESH_UI_CANNED_MAX 16U
#define MESH_UI_CANNED_TEXT_MAX 64U

/* Everything a backend needs to draw a cursor and the compose target. Lives in the store and
   is copied into each snapshot, so backends stay stateless. */
struct mesh_ui_nav {
    enum mesh_ui_screen screen;
    uint32_t cursor[MESH_UI_SCREEN_COUNT];
    /* Where the Compose tab sends: MESH_MESSAGE_BROADCAST_ADDR for the primary channel, or a
       node number picked from the Nodes or Messages tab. */
    uint32_t target_node;
    char target_name[MESH_UI_NAV_TARGET_NAME_MAX];
    /* One-line transient notice ("Sent to ABCD", "Connecting..."); empty when none. */
    char toast[MESH_UI_NAV_TOAST_MAX];
    uint64_t toast_until_ms;
    /* Message count at the last clamp, so a cursor parked on the newest message follows new
       traffic instead of being left behind. */
    uint32_t messages_seen;
};

enum mesh_ui_action_type {
    MESH_UI_ACTION_NONE = 0,
    MESH_UI_ACTION_CONNECT,   /* identifier = BLE address */
    MESH_UI_ACTION_SEND_TEXT, /* dest/channel/text */
};

struct mesh_ui_action {
    enum mesh_ui_action_type type;
    char identifier[64];
    uint32_t dest;
    uint8_t channel;
    char text[MESH_UI_CANNED_TEXT_MAX];
};

void mesh_ui_nav_init(struct mesh_ui_nav *nav);

/* Applies one button press. Returns true when the visible state changed. When the press
   asks the app to do something, *out_action is filled in (may be NULL to discard). The store
   is read for list sizes and to resolve node names; it is not modified. */
bool mesh_ui_nav_handle_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                            enum mesh_ui_key key, struct mesh_ui_action *out_action);

/* Keeps cursors inside their lists after the data changed. Returns true if anything moved. */
bool mesh_ui_nav_clamp(struct mesh_ui_nav *nav, const struct mesh_ui_store *store);

/* Rows on a screen for the current data (the Compose tab counts its "To:" row). */
uint32_t mesh_ui_nav_row_count(const struct mesh_ui_store *store, enum mesh_ui_screen screen);

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
