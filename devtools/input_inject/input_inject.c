/*
 * A virtual pad, for measuring the client with its own buttons.
 *
 * What this is for is the integrated latency number: a press to a frame, inside the real client,
 * with a tile being read and decoded and a BLE link being serviced. Taking it by hand works once;
 * taking it the same way twice does not, and a percentile wants a few hundred presses at a known
 * cadence rather than a thumb.
 *
 * So this creates a uinput device that reports what the Brick's own pad reports - the face
 * buttons through the BTN_ space with that case's positions, the d-pad as ABS_HAT0X/Y - and
 * plays a script of presses into it. The client finds it in its ordinary scan of
 * /dev/input/event*, maps it with the ordinary profile, and cannot tell it from the plastic:
 * every millisecond the probe measures is spent in the same evdev read, the same epoll loop and
 * the same present() a thumb would have gone through.
 *
 * Two things about the order it has to run in, both learned the hard way:
 *
 *   - The client scans for input devices once, at startup, so this must be running *before* the
 *     client starts. --after is what buys the time to start it.
 *   - A uinput device dies with the process that made it, so this must outlive the run. It
 *     stays up for --hold after the script finishes.
 *
 * Not part of the CMake build: it runs on the device rather than on the host, like
 * devtools/tile_bench. See devtools/input_inject/build.sh.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* How long a button is held down. Well under the client's 350 ms repeat delay, so a scripted
   press is one press and never the start of a hold. */
#define INJECT_HOLD_MS 30

struct inject_button {
    const char *name;
    uint16_t type; /* EV_KEY or EV_ABS */
    uint16_t code;
    int32_t value; /* the pressed value; EV_KEY is always 1 */
};

/*
 * The names on the case, and what this hardware reports for each.
 *
 * These are the `brick` row of src/ui/input/input_profile.c, deliberately duplicated rather than
 * included: this program is built on its own, outside the client's CMake, and a table of a
 * dozen codes is a smaller thing to keep in step than a build that reaches into src/. The
 * client's own test is what holds that table honest; what holds this one honest is that a wrong
 * code here presses the wrong button, visibly, in the first run.
 */
static const struct inject_button k_buttons[] = {
    {"a", EV_KEY, BTN_EAST, 1},  /* 305 - printed A, on the right */
    {"b", EV_KEY, BTN_SOUTH, 1}, /* 304 - printed B, at the bottom */
    {"x", EV_KEY, BTN_WEST, 1},  /* 308 - printed X, on top */
    {"y", EV_KEY, BTN_NORTH, 1}, /* 307 - printed Y, on the left */
    {"l1", EV_KEY, BTN_TL, 1},   /* 310 */
    {"r1", EV_KEY, BTN_TR, 1},   /* 311 */
    {"select", EV_KEY, BTN_SELECT, 1}, {"start", EV_KEY, BTN_START, 1},
    {"left", EV_ABS, ABS_HAT0X, -1},   {"right", EV_ABS, ABS_HAT0X, 1},
    {"up", EV_ABS, ABS_HAT0Y, -1},     {"down", EV_ABS, ABS_HAT0Y, 1},
};

static void sleep_ms(long ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

static bool emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event event;
    memset(&event, 0, sizeof event);
    event.type = type;
    event.code = code;
    event.value = value;
    return write(fd, &event, sizeof event) == (ssize_t)sizeof event;
}

static bool press(int fd, const struct inject_button *button) {
    bool ok = emit(fd, button->type, button->code, button->value);
    ok = emit(fd, EV_SYN, SYN_REPORT, 0) && ok;
    sleep_ms(INJECT_HOLD_MS);
    ok = emit(fd, button->type, button->code, 0) && ok;
    ok = emit(fd, EV_SYN, SYN_REPORT, 0) && ok;
    return ok;
}

static const struct inject_button *find_button(const char *name, size_t length) {
    for (size_t i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; ++i) {
        if (strlen(k_buttons[i].name) == length && strncmp(k_buttons[i].name, name, length) == 0) {
            return &k_buttons[i];
        }
    }
    return NULL;
}

/*
 * The device.
 *
 * The legacy `struct uinput_user_dev` write rather than UI_DEV_SETUP: the Brick's kernel is
 * 4.9, and the older path works on every kernel that has uinput at all. The absolute axes need
 * their ranges declared or the hat reports nothing; -1..1 is what a d-pad hat is.
 */
static int open_device(const char *name) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open /dev/uinput: %s\n", strerror(errno));
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        fprintf(stderr, "UI_SET_EVBIT: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    for (size_t i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; ++i) {
        const int request = k_buttons[i].type == EV_KEY ? UI_SET_KEYBIT : UI_SET_ABSBIT;
        if (ioctl(fd, request, k_buttons[i].code) < 0) {
            fprintf(stderr, "UI_SET_%sBIT %u: %s\n", k_buttons[i].type == EV_KEY ? "KEY" : "ABS",
                    k_buttons[i].code, strerror(errno));
            close(fd);
            return -1;
        }
    }

    struct uinput_user_dev setup;
    memset(&setup, 0, sizeof setup);
    snprintf(setup.name, sizeof setup.name, "%s", name);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1209; /* pid.codes, the open-hardware range: this is not a real pad */
    setup.id.product = 0x0001;
    setup.id.version = 1;
    for (int axis = ABS_HAT0X; axis <= ABS_HAT0Y; ++axis) {
        setup.absmin[axis] = -1;
        setup.absmax[axis] = 1;
    }
    if (write(fd, &setup, sizeof setup) != (ssize_t)sizeof setup) {
        fprintf(stderr, "uinput setup write: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "UI_DEV_CREATE: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [--after MS] [--every MS] [--hold MS] [--name NAME] TOKEN...\n"
            "\n"
            "  --after MS   wait this long before the first press, so the client can be\n"
            "               started while the device exists (default 15000)\n"
            "  --every MS   the gap between presses (default 250)\n"
            "  --hold MS    keep the device alive this long after the last press, so the\n"
            "               client it is driving does not lose its pad mid-run (default 5000)\n"
            "  --name NAME  what the device calls itself\n"
            "\n"
            "TOKEN is a button, optionally repeated: right, right:200.\n"
            "Buttons: a b x y l1 r1 select start up down left right, and `wait` for a pause\n"
            "of --every (so wait:4 is four beats of nothing). The separator is a colon rather\n"
            "than a star because a star is a glob and every shell in the path would eat it.\n",
            program);
}

int main(int argc, char **argv) {
    long after_ms = 15000;
    long every_ms = 250;
    long hold_ms = 5000;
    const char *name = "MeshClient input injector";

    int index = 1;
    for (; index < argc; ++index) {
        const char *arg = argv[index];
        if (strcmp(arg, "--after") == 0 && index + 1 < argc) {
            after_ms = strtol(argv[++index], NULL, 10);
        } else if (strcmp(arg, "--every") == 0 && index + 1 < argc) {
            every_ms = strtol(argv[++index], NULL, 10);
        } else if (strcmp(arg, "--hold") == 0 && index + 1 < argc) {
            hold_ms = strtol(argv[++index], NULL, 10);
        } else if (strcmp(arg, "--name") == 0 && index + 1 < argc) {
            name = argv[++index];
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strncmp(arg, "--", 2) == 0) {
            usage(argv[0]);
            return 2;
        } else {
            break;
        }
    }
    if (index >= argc) {
        usage(argv[0]);
        return 2;
    }

    /* The script is checked before the device is created, so a typo fails with the launcher
       still on screen rather than half way through a run. */
    for (int i = index; i < argc; ++i) {
        const char *token = argv[i];
        const char *star = strchr(token, ':');
        const size_t length = star != NULL ? (size_t)(star - token) : strlen(token);
        if (strncmp(token, "wait", length) == 0 && length == 4U) {
            continue;
        }
        if (find_button(token, length) == NULL) {
            fprintf(stderr, "unknown button '%.*s'\n", (int)length, token);
            return 2;
        }
    }

    const int fd = open_device(name);
    if (fd < 0) {
        return 1;
    }
    printf("READY %s\n", name);
    fflush(stdout);

    sleep_ms(after_ms);

    long pressed = 0;
    for (int i = index; i < argc; ++i) {
        const char *token = argv[i];
        const char *star = strchr(token, ':');
        const size_t length = star != NULL ? (size_t)(star - token) : strlen(token);
        long count = star != NULL ? strtol(star + 1, NULL, 10) : 1;
        const struct inject_button *const button = find_button(token, length);
        for (long n = 0; n < count; ++n) {
            if (button != NULL && !press(fd, button)) {
                fprintf(stderr, "write to the virtual pad failed: %s\n", strerror(errno));
                ioctl(fd, UI_DEV_DESTROY);
                close(fd);
                return 1;
            }
            if (button != NULL) {
                ++pressed;
            }
            sleep_ms(every_ms - (button != NULL ? INJECT_HOLD_MS : 0));
        }
    }

    printf("PRESSED %ld\n", pressed);
    fflush(stdout);
    sleep_ms(hold_ms);

    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    printf("DONE\n");
    return 0;
}
