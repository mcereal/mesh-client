#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/input.h"

#include "mesh/event_loop.h"
#include "mesh/log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

/* Standard evdev codes. The Brick reports its face and system buttons through the usual
   KEY_ and BTN_ space, so these work without a device-specific keymap. BTN_START is left out
   deliberately: it is the natural "confirm" button for the menu work still to come. */
#define MESH_UI_INPUT_MAX_QUIT_KEYS 16U

static const uint16_t k_default_quit_keys[] = {
    KEY_ESC,      /* 1   - USB keyboard, and what most emulators map "back" to */
    KEY_POWER,    /* 116 */
    KEY_MENU,     /* 139 - the Brick's MENU button, the NextUI convention for leaving a pak */
    BTN_SELECT,   /* 314 */
    BTN_MODE,     /* 316 - "guide"/menu on gamepad-style reports */
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
            mesh_log_info("input", "Quit keys overridden by MESHCLIENT_QUIT_KEYS (%zu codes)", s_quit_key_count);
            return;
        }
        mesh_log_warn("input", "MESHCLIENT_QUIT_KEYS='%s' parsed to nothing; using defaults", override);
    }

    for (size_t i = 0; i < sizeof(k_default_quit_keys) / sizeof(k_default_quit_keys[0]); ++i) {
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
            /* value 1 is a press, 2 is autorepeat, 0 is a release. */
            if (batch[i].type != EV_KEY || batch[i].value != 1) {
                continue;
            }

            /* Logged at info so a device run reveals the real button codes in
               MeshClient.txt, which is how MESHCLIENT_QUIT_KEYS gets tuned. */
            mesh_log_info("input", "key code %u pressed", (unsigned)batch[i].code);

            if (mesh_ui_input_is_quit_key(batch[i].code)) {
                mesh_log_info("input", "Quit key %u pressed; stopping", (unsigned)batch[i].code);
                mesh_event_loop_request_stop(input->loop);
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

        const int add_result = mesh_event_loop_add_fd(loop, fd, EPOLLIN, mesh_ui_input_event_callback, input);
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
        mesh_log_info("input", "Watching %zu input device(s); %s", input->count, mesh_ui_input_quit_hint());
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
}
