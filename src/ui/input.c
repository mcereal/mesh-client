#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/input.h"

#include "mesh/core/event_loop.h"
#include "mesh/utils/array.h"
#include "mesh/utils/log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
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
static char s_quit_hint[64];

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
            snprintf(s_quit_hint, sizeof s_quit_hint, "Quit: key code %u", s_quit_keys[0]);
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
    snprintf(s_quit_hint, sizeof s_quit_hint, "Press MENU to quit");
}

void mesh_ui_input_reload_quit_keys(void) {
    s_quit_keys_loaded = false;
    s_quit_key_count = 0U;
    memset(s_quit_keys, 0, sizeof s_quit_keys);
    memset(s_quit_hint, 0, sizeof s_quit_hint);
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

const char *mesh_ui_input_quit_hint(void) {
    mesh_ui_input_load_quit_keys();
    return s_quit_hint;
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
    if (input == NULL) {
        return;
    }

    enum mesh_ui_key key = MESH_UI_KEY_NONE;
    if (type == EV_KEY) {
        /* value 1 is a press, 2 is autorepeat, 0 is a release. Repeat is honoured for the
           navigation keys so holding the d-pad scrolls, but never for quitting. */
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
    } else if (type == EV_ABS) {
        key = mesh_ui_input_map_hat(code, value);
    }

    if (key != MESH_UI_KEY_NONE && input->on_key != NULL) {
        input->on_key(input->key_userdata, key);
    }
}

static int mesh_ui_input_event_callback(int fd, uint32_t events, void *userdata) {
    struct mesh_ui_input *input = (struct mesh_ui_input *)userdata;
    if (input == NULL || (events & EPOLLIN) == 0U) {
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
            break;
        }
        if (bytes == 0) {
            break;
        }

        const size_t count = (size_t)bytes / sizeof(struct input_event);
        for (size_t i = 0; i < count; ++i) {
            mesh_ui_input_handle_event(input, batch[i].type, batch[i].code, batch[i].value);
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

int mesh_ui_input_init(struct mesh_ui_input *input, struct mesh_event_loop *loop) {
    if (input == NULL || loop == NULL) {
        return -EINVAL;
    }

    memset(input, 0, sizeof *input);
    input->loop = loop;
    mesh_ui_input_load_quit_keys();

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

    input->count = 0U;
    input->loop = NULL;
    input->on_key = NULL;
    input->key_userdata = NULL;
}
