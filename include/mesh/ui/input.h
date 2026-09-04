#pragma once

#include "mesh/ui/nav.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

typedef void (*mesh_ui_key_handler)(void *userdata, enum mesh_ui_key key);

#define MESH_UI_INPUT_MAX_DEVICES 8U

/* evdev button reader. The TrimUI Brick has no keyboard and no console, so this is the only
   way to drive the client from the device. Every /dev/input/event* node is watched; a press of
   any quit key asks the event loop to stop, and everything else that maps to a logical key
   (face buttons, shoulders, d-pad hat axes, and the arrow/Enter keys of a USB keyboard) goes
   to the handler. */
struct mesh_ui_input {
    struct mesh_event_loop *loop;
    int fds[MESH_UI_INPUT_MAX_DEVICES];
    size_t count;
    mesh_ui_key_handler on_key;
    void *key_userdata;
};

/* Never fails the caller: a host with no readable /dev/input (the dev container, CI) simply
   watches nothing. Returns the number of devices opened. */
int mesh_ui_input_init(struct mesh_ui_input *input, struct mesh_event_loop *loop);
void mesh_ui_input_shutdown(struct mesh_ui_input *input);

void mesh_ui_input_set_handler(struct mesh_ui_input *input, mesh_ui_key_handler handler,
                               void *userdata);

/* One raw evdev event (type/code/value as in struct input_event). Public so the mapping can be
   tested without a device: quit keys stop the loop, everything else is translated and handed
   to the handler. */
void mesh_ui_input_handle_event(struct mesh_ui_input *input, uint16_t type, uint16_t code,
                                int32_t value);

/* evdev key code (or hat axis code with its direction) to logical key; MESH_UI_KEY_NONE when
   the code has no meaning for the UI. */
enum mesh_ui_key mesh_ui_input_map_key(uint16_t code);
enum mesh_ui_key mesh_ui_input_map_hat(uint16_t code, int32_t value);

/* True when the evdev key code should quit. Defaults to MENU/POWER/ESC/MODE/SELECT and can be
   replaced with a comma-separated list of decimal codes in MESHCLIENT_QUIT_KEYS. */
bool mesh_ui_input_is_quit_key(uint16_t code);

/* Footer text for the UI backends, e.g. "Press MENU to quit". */
const char *mesh_ui_input_quit_hint(void);

/* The quit-key set is parsed from the environment once and cached. Exposed so tests can
   re-read MESHCLIENT_QUIT_KEYS after changing it; not needed in normal use. */
void mesh_ui_input_reload_quit_keys(void);

#ifdef __cplusplus
}
#endif
