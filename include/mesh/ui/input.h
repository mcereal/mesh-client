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

    /* Software key repeat for a held direction. `repeat_timer_fd` is <= 0 when there is no
       timer - a zeroed struct (what the tests use) and a host where timerfd_create failed both
       land there, and repeat then simply never fires on its own. `repeat_type`/`repeat_code`
       are the raw evdev event that started the hold, kept so the matching release ends it. */
    int repeat_timer_fd;
    enum mesh_ui_key repeat_key;
    uint16_t repeat_type;
    uint16_t repeat_code;
    unsigned int repeat_count;
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

/* How long the next repeat of a held direction waits, given how many repeats it has already
   produced: 0 asks for the initial hold delay, and the interval ramps down after a few rows so
   a long roster does not take a minute to walk. Returns 0 when repeat is switched off with
   MESHCLIENT_KEY_REPEAT_DELAY_MS=0. Pure, and public so the ramp is testable off-device. */
unsigned int mesh_ui_input_repeat_delay_ms(unsigned int repeats);

/* The direction being held, or MESH_UI_KEY_NONE when nothing is repeating. */
enum mesh_ui_key mesh_ui_input_repeat_key(const struct mesh_ui_input *input);

/* Emits one repeat of the held key and schedules the next. The repeat timer calls this; it is
   public so a test can step a hold without a real timerfd. */
void mesh_ui_input_repeat_tick(struct mesh_ui_input *input);

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

/* The same, for MESHCLIENT_KEY_REPEAT_DELAY_MS and MESHCLIENT_KEY_REPEAT_MS. */
void mesh_ui_input_reload_key_repeat(void);

#ifdef __cplusplus
}
#endif
