#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

#define MESH_UI_INPUT_MAX_DEVICES 8U

/* evdev button reader. The TrimUI Brick has no keyboard and no console, so without this the
   foreground loop has no way to be stopped from the device itself. Every /dev/input/event*
   node is watched; a press of any quit key asks the event loop to stop. */
struct mesh_ui_input {
    struct mesh_event_loop *loop;
    int fds[MESH_UI_INPUT_MAX_DEVICES];
    size_t count;
};

/* Never fails the caller: a host with no readable /dev/input (the dev container, CI) simply
   watches nothing. Returns the number of devices opened. */
int mesh_ui_input_init(struct mesh_ui_input *input, struct mesh_event_loop *loop);
void mesh_ui_input_shutdown(struct mesh_ui_input *input);

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
