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

/*
 * How many nodes may be watched at once.
 *
 * Eight was one per node the Brick has, twice over. It is the wrong number for a host that also
 * has a keyboard, a mouse, two trackpads and an accelerometer - not because watching sixteen
 * costs anything, but because the scan stops at the cap, and a pad enumerated after the cap is
 * a client with no buttons. The filter in mesh_ui_input_init() is what usually keeps the count
 * far below this; the cap is the backstop.
 */
#define MESH_UI_INPUT_MAX_DEVICES 16U

/* evdev button reader. The TrimUI Brick has no keyboard and no console, so this is the only
   way to drive the client from the device. Every /dev/input/event* node that reports a button
   this client maps is watched - see mesh_ui_input_init() for why the ones that do not are
   dropped rather than watched anyway; a press of any quit key asks the event loop to stop, and
   everything else that maps to a logical key (face buttons, shoulders, d-pad hat axes, and the
   arrow/Enter keys of a USB keyboard) goes to the handler. */
struct mesh_ui_input {
    struct mesh_event_loop *loop;
    int fds[MESH_UI_INPUT_MAX_DEVICES];
    size_t count;
    mesh_ui_key_handler on_key;
    void *key_userdata;

    /* Software key repeat for a held direction. `repeat_timer_fd` is <= 0 when there is no
       timer - a zeroed struct (what the tests use) and a host where timerfd_create failed both
       land there, and repeat then simply never fires on its own. `repeat_type`/`repeat_code`
       are the raw evdev event that started the hold, kept so the matching release ends it, and
       `repeat_source_fd` the device it came from, so a hold ends when that device goes away. */
    int repeat_timer_fd;
    int repeat_source_fd;
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

/* The same, naming the device fd the event arrived on so a hold can be tied to it. Anything
   that is not reading a real device passes -1, which is what the call above does. */
void mesh_ui_input_handle_device_event(struct mesh_ui_input *input, int source_fd, uint16_t type,
                                       uint16_t code, int32_t value);

/* The device behind `source_fd` is gone - unplugged, or its fd went bad. Ends a hold that
   started there: its release will never arrive, and a repeat with no release scrolls forever. */
void mesh_ui_input_device_lost(struct mesh_ui_input *input, int source_fd);

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

/*
 * The same fact as a keycap: "MENU", or "K139" when MESHCLIENT_QUIT_KEYS has moved quitting to
 * a key whose name we do not know.
 *
 * What the action bar draws inside the pill, where the hint above is a sentence for the line
 * that reports the transport. Never NULL, and never translated - a cap is what is printed on
 * the case. See enum mesh_ui_button.
 */
const char *mesh_ui_input_quit_cap(void);

/*
 * Whether this client would do anything with an evdev key code: it maps to a logical key under
 * the selected profile, or it quits. What the device filter asks about each code a node claims
 * to report.
 */
bool mesh_ui_input_reads_code(uint16_t code);

/*
 * Whether a node reporting these capability bitmaps is worth watching - the decision behind the
 * filter in mesh_ui_input_init(), with the ioctls taken off it so it can be exercised without a
 * device.
 *
 * `key_bits` and `abs_bits` are EVIOCGBIT bitmaps for EV_KEY and EV_ABS, and NULL means the
 * node could not say. A node that could not say either is wanted: being unable to tell is not
 * evidence of a useless device, and a dropped pad is a client nobody can drive, where a spare
 * fd on something silent costs nothing.
 */
bool mesh_ui_input_device_wanted(const unsigned long *key_bits, size_t key_words,
                                 const unsigned long *abs_bits, size_t abs_words);

/* Sizing for the two bitmaps above, in the units EVIOCGBIT fills. */
#define MESH_UI_INPUT_BITS_PER_LONG (8U * (unsigned)sizeof(unsigned long))
#define MESH_UI_INPUT_BIT_WORDS(count)                                                             \
    (((count) + MESH_UI_INPUT_BITS_PER_LONG - 1U) / MESH_UI_INPUT_BITS_PER_LONG)

/* The quit-key set is parsed from the environment once and cached. Exposed so tests can
   re-read MESHCLIENT_QUIT_KEYS after changing it; not needed in normal use. */
void mesh_ui_input_reload_quit_keys(void);

/* The same, for MESHCLIENT_KEY_REPEAT_DELAY_MS and MESHCLIENT_KEY_REPEAT_MS. */
void mesh_ui_input_reload_key_repeat(void);

#ifdef __cplusplus
}
#endif
