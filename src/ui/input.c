#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/input.h"

#include "mesh/core/event_loop.h"
#include "mesh/i18n/strings.h"
#include "mesh/utils/array.h"
#include "mesh/utils/env.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* Standard evdev codes. The Brick's gamepad device ("TRIMUI Player1") reports the face and
   system buttons through the usual BTN_ space and the d-pad as ABS_HAT0X/Y, so these work
   without a device-specific keymap. SELECT and START are not quit keys: they sit next to the
   d-pad and are too easy to hit while navigating. */
#define MESH_UI_INPUT_MAX_QUIT_KEYS 16U

static const uint16_t k_default_quit_keys[] = {
    KEY_ESC,   /* 1   - USB keyboard, and what most emulators map "back" to */
    KEY_POWER, /* 116 */
    KEY_MENU,  /* 139 - the Brick's MENU button, the NextUI convention for leaving a pak */
    BTN_MODE,  /* 316 - the same MENU button as the gamepad device reports it */
};

/* Parsed once from MESHCLIENT_QUIT_KEYS so the mapping can be corrected on-device without a
   rebuild: MESHCLIENT_QUIT_KEYS="139,316" meshclient ... */
static uint16_t s_quit_keys[MESH_UI_INPUT_MAX_QUIT_KEYS];
static size_t s_quit_key_count;
static bool s_quit_keys_loaded;
/* The hint's text is rebuilt per call (see mesh_ui_input_quit_hint); only which of the two
   forms it takes is settled when the keys are loaded. */
static char s_quit_hint[64];
/* The keycap form of the same fact - "MENU", or "K139" when somebody has rebound it. Separate
   from the hint because it is not prose: a cap is what is printed on the plastic, and a key
   code stands in when there is no plastic to read it off. */
static char s_quit_cap[8];
static bool s_quit_hint_is_key_code;

static void mesh_ui_input_load_quit_keys(void) {
    if (s_quit_keys_loaded) {
        return;
    }
    s_quit_keys_loaded = true;

    const char *override = getenv("MESHCLIENT_QUIT_KEYS");
    if (override != NULL && override[0] != '\0') {
        const char *cursor = override;
        while (*cursor != '\0' && s_quit_key_count < MESH_UI_INPUT_MAX_QUIT_KEYS) {
            char *end = NULL;
            const long value = strtol(cursor, &end, 10);
            if (end == cursor) {
                break;
            }
            if (value > 0 && value <= UINT16_MAX) {
                s_quit_keys[s_quit_key_count++] = (uint16_t)value;
            }
            cursor = end;
            while (*cursor == ',' || *cursor == ' ') {
                ++cursor;
            }
        }

        if (s_quit_key_count > 0U) {
            s_quit_hint_is_key_code = true;
            mesh_log_info("input", "Quit keys overridden by MESHCLIENT_QUIT_KEYS (%zu codes)",
                          s_quit_key_count);
            return;
        }
        mesh_log_warn("input", "MESHCLIENT_QUIT_KEYS='%s' parsed to nothing; using defaults",
                      override);
    }

    for (size_t i = 0; i < MESH_ARRAY_LEN(k_default_quit_keys); ++i) {
        s_quit_keys[s_quit_key_count++] = k_default_quit_keys[i];
    }
    s_quit_hint_is_key_code = false;
}

/*
 * Software key repeat.
 *
 * The kernel's own autorepeat is an EV_KEY/EV_REP feature, and the Brick reports its d-pad as
 * the absolute axes ABS_HAT0X/Y. An absolute axis never repeats however long it is held: the
 * driver sends one event on the way out of centre and one on the way back, so a 60-node roster
 * cost 60 separate presses. The repeat is therefore generated here, from a timerfd on the same
 * event loop, and it covers the arrow keys of a USB keyboard too - where the kernel would
 * repeat, ours takes over, so both devices scroll at the same speed.
 *
 * Only the four directions repeat. A, B, X and Y confirm or go back, and a held confirm that
 * fired forty times would be a trap rather than a convenience.
 */
#define MESH_UI_INPUT_REPEAT_DELAY_MS 350U /* held this long before the first repeat */
#define MESH_UI_INPUT_REPEAT_MS 90U        /* then a row this often */
#define MESH_UI_INPUT_REPEAT_RAMP 8U       /* rows before the interval halves */
#define MESH_UI_INPUT_REPEAT_MIN_MS 25U    /* never faster than this, whatever the knobs say */

static unsigned int s_repeat_delay_ms;
static unsigned int s_repeat_interval_ms;
static bool s_repeat_loaded;

static void mesh_ui_input_load_key_repeat(void) {
    if (s_repeat_loaded) {
        return;
    }
    s_repeat_loaded = true;
    /* Tunable on-device from launch.sh, the same escape hatch MESHCLIENT_QUIT_KEYS is; a delay
       of 0 turns hold-to-scroll off and restores one row per press. */
    s_repeat_delay_ms = (unsigned int)mesh_env_int("MESHCLIENT_KEY_REPEAT_DELAY_MS", 0, 5000,
                                                   MESH_UI_INPUT_REPEAT_DELAY_MS);
    s_repeat_interval_ms =
        (unsigned int)mesh_env_int("MESHCLIENT_KEY_REPEAT_MS", 10, 2000, MESH_UI_INPUT_REPEAT_MS);
}

void mesh_ui_input_reload_key_repeat(void) {
    s_repeat_loaded = false;
    s_repeat_delay_ms = 0U;
    s_repeat_interval_ms = 0U;
    mesh_ui_input_load_key_repeat();
}

unsigned int mesh_ui_input_repeat_delay_ms(unsigned int repeats) {
    mesh_ui_input_load_key_repeat();
    if (s_repeat_delay_ms == 0U) {
        return 0U;
    }
    if (repeats == 0U) {
        return s_repeat_delay_ms;
    }
    if (repeats < MESH_UI_INPUT_REPEAT_RAMP) {
        return s_repeat_interval_ms;
    }

    /* Past the ramp the finger is clearly travelling, not nudging, so the list speeds up. The
       clamps keep a hand-set interval from being made slower by the acceleration. */
    unsigned int fast = s_repeat_interval_ms / 2U;
    if (fast < MESH_UI_INPUT_REPEAT_MIN_MS) {
        fast = MESH_UI_INPUT_REPEAT_MIN_MS;
    }
    if (fast > s_repeat_interval_ms) {
        fast = s_repeat_interval_ms;
    }
    return fast;
}

static bool mesh_ui_input_key_repeats(enum mesh_ui_key key) {
    return key == MESH_UI_KEY_UP || key == MESH_UI_KEY_DOWN || key == MESH_UI_KEY_LEFT ||
           key == MESH_UI_KEY_RIGHT;
}

/* True when the hold in progress was started by this exact evdev event, which is what makes a
   release end it. A zeroed struct reports MESH_UI_KEY_NONE and so never matches. */
static bool mesh_ui_input_repeat_owns(const struct mesh_ui_input *input, uint16_t type,
                                      uint16_t code) {
    return input->repeat_key != MESH_UI_KEY_NONE && input->repeat_type == type &&
           input->repeat_code == code;
}

/* One-shot each time rather than an interval timer, because the delay changes as the hold
   ramps up. An all-zero it_value disarms, which is exactly what a released key wants. */
static void mesh_ui_input_repeat_schedule(struct mesh_ui_input *input) {
    if (input->repeat_timer_fd <= 0) {
        return;
    }

    const unsigned int ms = input->repeat_key == MESH_UI_KEY_NONE
                                ? 0U
                                : mesh_ui_input_repeat_delay_ms(input->repeat_count);

    struct itimerspec spec;
    memset(&spec, 0, sizeof spec);
    spec.it_value.tv_sec = (time_t)(ms / 1000U);
    spec.it_value.tv_nsec = (long)(ms % 1000U) * 1000000L;
    if (timerfd_settime(input->repeat_timer_fd, 0, &spec, NULL) < 0) {
        mesh_log_warn("input", "key repeat timerfd_settime failed: %s", strerror(errno));
    }
}

static void mesh_ui_input_repeat_cancel(struct mesh_ui_input *input) {
    if (input->repeat_key == MESH_UI_KEY_NONE) {
        return;
    }
    input->repeat_key = MESH_UI_KEY_NONE;
    input->repeat_type = 0U;
    input->repeat_code = 0U;
    input->repeat_source_fd = -1;
    input->repeat_count = 0U;
    mesh_ui_input_repeat_schedule(input);
}

/* Every press goes through here, so anything that is not a repeatable direction - a face
   button, an unmapped code - also ends whatever was being held. That keeps a missed release
   from scrolling forever, and makes "press A" a definite stop rather than a maybe. */
static void mesh_ui_input_repeat_start(struct mesh_ui_input *input, enum mesh_ui_key key,
                                       uint16_t type, uint16_t code, int source_fd) {
    if (!mesh_ui_input_key_repeats(key) || mesh_ui_input_repeat_delay_ms(0U) == 0U) {
        mesh_ui_input_repeat_cancel(input);
        return;
    }

    input->repeat_key = key;
    input->repeat_type = type;
    input->repeat_code = code;
    input->repeat_source_fd = source_fd;
    input->repeat_count = 0U;
    mesh_ui_input_repeat_schedule(input);
}

/* The only release that never arrives is the one from a device that is no longer there: a
   keyboard unplugged mid-hold hangs up its fd instead of sending the key up, and without this
   the timer would happily scroll the list until some other button was pressed. */
void mesh_ui_input_device_lost(struct mesh_ui_input *input, int source_fd) {
    if (input == NULL || source_fd < 0 || input->repeat_source_fd != source_fd) {
        return;
    }
    mesh_ui_input_repeat_cancel(input);
}

void mesh_ui_input_repeat_tick(struct mesh_ui_input *input) {
    if (input == NULL || input->repeat_key == MESH_UI_KEY_NONE) {
        return;
    }

    const enum mesh_ui_key key = input->repeat_key;
    if (input->repeat_count < UINT_MAX) {
        input->repeat_count++;
    }
    /* Scheduled before the handler runs: the handler owns the UI and may tear this input down,
       and by then the timer must already be set (or not) for the next row. */
    mesh_ui_input_repeat_schedule(input);
    if (input->on_key != NULL) {
        input->on_key(input->key_userdata, key);
    }
}

enum mesh_ui_key mesh_ui_input_repeat_key(const struct mesh_ui_input *input) {
    return input == NULL ? MESH_UI_KEY_NONE : input->repeat_key;
}

static int mesh_ui_input_repeat_callback(int fd, uint32_t events, void *userdata) {
    struct mesh_ui_input *input = (struct mesh_ui_input *)userdata;
    if (input == NULL || (events & EPOLLIN) == 0U) {
        return 0;
    }

    uint64_t expirations = 0U;
    ssize_t bytes;
    do {
        bytes = read(fd, &expirations, sizeof expirations);
    } while (bytes < 0 && errno == EINTR);

    mesh_ui_input_repeat_tick(input);
    return 0;
}

void mesh_ui_input_reload_quit_keys(void) {
    s_quit_keys_loaded = false;
    s_quit_key_count = 0U;
    memset(s_quit_keys, 0, sizeof s_quit_keys);
    memset(s_quit_hint, 0, sizeof s_quit_hint);
    memset(s_quit_cap, 0, sizeof s_quit_cap);
    s_quit_hint_is_key_code = false;
    mesh_ui_input_load_quit_keys();
}

bool mesh_ui_input_is_quit_key(uint16_t code) {
    mesh_ui_input_load_quit_keys();
    for (size_t i = 0; i < s_quit_key_count; ++i) {
        if (s_quit_keys[i] == code) {
            return true;
        }
    }
    return false;
}

/*
 * Built on every call rather than cached beside the key codes.
 *
 * The codes are settled once at startup and never change; the language is not the same kind of
 * fact, and a hint formatted in English when the input layer came up would outlive a switch to
 * another one. The buffer is still file-scope, so the pointer's lifetime is what it always was.
 */
const char *mesh_ui_input_quit_hint(void) {
    mesh_ui_input_load_quit_keys();
    if (s_quit_hint_is_key_code) {
        mesh_str_format(s_quit_hint, sizeof s_quit_hint, MESH_STR_HINT_QUIT_KEY_CODE,
                        s_quit_keys[0]);
    } else {
        mesh_str_copy(s_quit_hint, sizeof s_quit_hint, mesh_str(MESH_STR_HINT_QUIT_MENU));
    }
    return s_quit_hint;
}

const char *mesh_ui_input_quit_cap(void) {
    mesh_ui_input_load_quit_keys();
    if (!s_quit_hint_is_key_code) {
        return "MENU";
    }
    /* A code rather than a name, prefixed so it is read as a key and not as a quantity. The
       buffer is file-scope for the same reason the hint's is: the pointer outlives the call. */
    snprintf(s_quit_cap, sizeof s_quit_cap, "K%u", (unsigned)s_quit_keys[0]);
    return s_quit_cap;
}

enum mesh_ui_key mesh_ui_input_map_key(uint16_t code) {
    switch (code) {
    /* Gamepad face buttons on the Brick, every one of them confirmed from the device log by
       pressing it and reading back the code. The button printed A is on the right (BTN_EAST,
       305) and B is at the bottom (BTN_SOUTH, 304) - the reverse of what the BTN_A/BTN_B
       aliases suggest. X and Y are stranger still: they do not follow the positional
       convention the other two do. The button printed Y, on the LEFT, reports BTN_NORTH (307,
       nominally "top"), so X on the top reports BTN_WEST (308). Mapping these by position
       leaves Y unreachable and every Y binding - saving a settings section, above all -
       firing X instead. Do not "correct" this back to the positional reading. */
    case BTN_EAST: /* 305, the Brick's A */
    case KEY_ENTER:
        return MESH_UI_KEY_A;
    case BTN_SOUTH: /* 304, B */
    case KEY_BACKSPACE:
        return MESH_UI_KEY_B;
    case BTN_WEST: /* 308, the Brick's X (top) */
        return MESH_UI_KEY_X;
    case BTN_NORTH: /* 307, the Brick's Y (left) */
    case KEY_SPACE:
        return MESH_UI_KEY_Y;
    case BTN_TL: /* 310 */
    case KEY_PAGEUP:
        return MESH_UI_KEY_L1;
    case BTN_TR: /* 311 */
    case KEY_PAGEDOWN:
    case KEY_TAB:
        return MESH_UI_KEY_R1;
    case BTN_SELECT: /* 314 */
        return MESH_UI_KEY_SELECT;
    case BTN_START: /* 315 */
        return MESH_UI_KEY_START;
    /* Keyboards, and d-pads that some drivers report as keys rather than a hat. */
    case KEY_UP:
    case BTN_DPAD_UP:
        return MESH_UI_KEY_UP;
    case KEY_DOWN:
    case BTN_DPAD_DOWN:
        return MESH_UI_KEY_DOWN;
    case KEY_LEFT:
    case BTN_DPAD_LEFT:
        return MESH_UI_KEY_LEFT;
    case KEY_RIGHT:
    case BTN_DPAD_RIGHT:
        return MESH_UI_KEY_RIGHT;
    default:
        return MESH_UI_KEY_NONE;
    }
}

enum mesh_ui_key mesh_ui_input_map_hat(uint16_t code, int32_t value) {
    /* 0 is the release back to centre; only the edge into a direction counts as a press. */
    if (value == 0) {
        return MESH_UI_KEY_NONE;
    }
    if (code == ABS_HAT0X) {
        return value < 0 ? MESH_UI_KEY_LEFT : MESH_UI_KEY_RIGHT;
    }
    if (code == ABS_HAT0Y) {
        return value < 0 ? MESH_UI_KEY_UP : MESH_UI_KEY_DOWN;
    }
    return MESH_UI_KEY_NONE;
}

void mesh_ui_input_set_handler(struct mesh_ui_input *input, mesh_ui_key_handler handler,
                               void *userdata) {
    if (input == NULL) {
        return;
    }
    input->on_key = handler;
    input->key_userdata = userdata;
}

void mesh_ui_input_handle_event(struct mesh_ui_input *input, uint16_t type, uint16_t code,
                                int32_t value) {
    mesh_ui_input_handle_device_event(input, -1, type, code, value);
}

void mesh_ui_input_handle_device_event(struct mesh_ui_input *input, int source_fd, uint16_t type,
                                       uint16_t code, int32_t value) {
    if (input == NULL) {
        return;
    }

    enum mesh_ui_key key = MESH_UI_KEY_NONE;
    if (type == EV_KEY) {
        /* value 1 is a press, 2 is the kernel's autorepeat, 0 is a release. */
        if (value == 0) {
            if (mesh_ui_input_repeat_owns(input, type, code)) {
                mesh_ui_input_repeat_cancel(input);
            }
            return;
        }
        if (value == 1) {
            if (mesh_ui_input_is_quit_key(code)) {
                mesh_log_info("input", "Quit key %u pressed; stopping", (unsigned)code);
                if (input->loop != NULL) {
                    mesh_event_loop_request_stop(input->loop);
                }
                return;
            }
            /* Logged at debug so a device run reveals the real button codes in
               MeshClient.txt, which is how MESHCLIENT_QUIT_KEYS gets tuned. */
            mesh_log_debug("input", "key code %u pressed", (unsigned)code);
        } else if (value != 2) {
            return;
        }
        key = mesh_ui_input_map_key(code);
        /* A direction is repeated by our timer or by nothing at all, never by the kernel: a
           keyboard whose autorepeat we also honoured would take two rows per step, and with
           MESHCLIENT_KEY_REPEAT_DELAY_MS=0 it would still scroll on hold after the knob
           promised it would not. Face buttons keep whatever the kernel does with them. */
        if (value == 2 && mesh_ui_input_key_repeats(key)) {
            return;
        }
    } else if (type == EV_ABS) {
        /* The hat back at centre: the release of whichever direction was held. */
        if (value == 0) {
            if (mesh_ui_input_repeat_owns(input, type, code)) {
                mesh_ui_input_repeat_cancel(input);
            }
            return;
        }
        key = mesh_ui_input_map_hat(code, value);
    }

    if (key == MESH_UI_KEY_NONE) {
        return;
    }

    mesh_ui_input_repeat_start(input, key, type, code, source_fd);
    if (input->on_key != NULL) {
        input->on_key(input->key_userdata, key);
    }
}

static int mesh_ui_input_event_callback(int fd, uint32_t events, void *userdata) {
    struct mesh_ui_input *input = (struct mesh_ui_input *)userdata;
    if (input == NULL) {
        return 0;
    }

    /* Hang-up is how an unplugged device says goodbye, and it comes with no EPOLLIN. */
    if ((events & (EPOLLHUP | EPOLLERR)) != 0U) {
        mesh_ui_input_device_lost(input, fd);
    }
    if ((events & EPOLLIN) == 0U) {
        return 0;
    }

    struct input_event batch[16];
    for (;;) {
        const ssize_t bytes = read(fd, batch, sizeof batch);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            mesh_log_warn("input", "read from input fd %d failed: %s", fd, strerror(errno));
            mesh_ui_input_device_lost(input, fd);
            break;
        }
        if (bytes == 0) {
            mesh_ui_input_device_lost(input, fd);
            break;
        }

        const size_t count = (size_t)bytes / sizeof(struct input_event);
        for (size_t i = 0; i < count; ++i) {
            mesh_ui_input_handle_device_event(input, fd, batch[i].type, batch[i].code,
                                              batch[i].value);
            if (input->loop != NULL && input->loop->stop_requested) {
                return 0;
            }
        }

        if ((size_t)bytes < sizeof batch) {
            break;
        }
    }

    return 0;
}

/* Repeat is a convenience, so a host without a spare fd loses hold-to-scroll and keeps every
   press working, rather than failing the client's startup. */
static void mesh_ui_input_setup_repeat_timer(struct mesh_ui_input *input,
                                             struct mesh_event_loop *loop) {
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        mesh_log_warn("input", "key repeat timerfd_create failed: %s", strerror(errno));
        return;
    }

    const int add_result =
        mesh_event_loop_add_fd(loop, fd, EPOLLIN, mesh_ui_input_repeat_callback, input);
    if (add_result < 0) {
        mesh_log_warn("input", "Failed to watch the key repeat timer: %d", add_result);
        close(fd);
        return;
    }

    input->repeat_timer_fd = fd;
}

int mesh_ui_input_init(struct mesh_ui_input *input, struct mesh_event_loop *loop) {
    if (input == NULL || loop == NULL) {
        return -EINVAL;
    }

    memset(input, 0, sizeof *input);
    input->loop = loop;
    input->repeat_timer_fd = -1;
    input->repeat_source_fd = -1;
    mesh_ui_input_load_quit_keys();
    mesh_ui_input_load_key_repeat();
    mesh_ui_input_setup_repeat_timer(input, loop);

    for (unsigned int index = 0; index < 32U && input->count < MESH_UI_INPUT_MAX_DEVICES; ++index) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%u", index);

        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        const int add_result =
            mesh_event_loop_add_fd(loop, fd, EPOLLIN, mesh_ui_input_event_callback, input);
        if (add_result < 0) {
            mesh_log_warn("input", "Failed to watch %s: %d", path, add_result);
            close(fd);
            continue;
        }

        mesh_log_debug("input", "Watching %s", path);
        input->fds[input->count++] = fd;
    }

    if (input->count == 0U) {
        /* Expected in the dev container and in CI; only the device really has buttons. */
        mesh_log_warn("input", "No readable /dev/input devices; buttons will not quit the client");
    } else {
        mesh_log_info("input", "Watching %zu input device(s); %s", input->count,
                      mesh_ui_input_quit_hint());
    }

    return (int)input->count;
}

void mesh_ui_input_shutdown(struct mesh_ui_input *input) {
    if (input == NULL) {
        return;
    }

    for (size_t i = 0; i < input->count; ++i) {
        if (input->loop != NULL) {
            mesh_event_loop_remove_fd(input->loop, input->fds[i]);
        }
        close(input->fds[i]);
    }

    if (input->repeat_timer_fd > 0) {
        if (input->loop != NULL) {
            mesh_event_loop_remove_fd(input->loop, input->repeat_timer_fd);
        }
        close(input->repeat_timer_fd);
    }
    input->repeat_timer_fd = -1;
    input->repeat_source_fd = -1;
    input->repeat_key = MESH_UI_KEY_NONE;
    input->repeat_count = 0U;

    input->count = 0U;
    input->loop = NULL;
    input->on_key = NULL;
    input->key_userdata = NULL;
}
