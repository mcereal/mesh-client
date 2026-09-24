#define _POSIX_C_SOURCE 200809L

/*
 * The control socket (include/mesh/app/control.h), spoken to the way `--ui-send` speaks to it:
 * a line in, a line back, over a real Unix socket on a real loop.
 *
 * What is held here is what a driver relies on without looking: that a key goes through the
 * path a button takes, that a bad name presses nothing, that a picture waits for the panel to
 * stop moving, and that the socket never deletes a file that is not its own.
 */

#include "framework/mesh_test.h"

#include "inkcell/ui/backend.h"
#include "inkcell/ui/fb_draw.h"
#include "inkwell/runtime/loop.h"
#include "mesh/app/control.h"
#include "mesh/core/config.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/route.h"
#include "mesh/ui/store.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* A backend with a frame to give and a switch for whether it is still moving. */
struct control_test_backend {
    bool moving;
    unsigned presents;
    uint32_t pixels[2];
};

static void control_test_present(void *state, const void *snapshot, void *userdata) {
    (void)state;
    (void)snapshot;
    ((struct control_test_backend *)userdata)->presents += 1U;
}

static bool control_test_moving(void *state, void *userdata) {
    (void)state;
    return ((struct control_test_backend *)userdata)->moving;
}

static bool control_test_frame(void *state, void *userdata, struct inkcell_surface *out) {
    (void)state;
    struct control_test_backend *const backend = (struct control_test_backend *)userdata;
    if (backend->presents == 0U) {
        return false;
    }
    *out = (struct inkcell_surface){
        .pixels = (uint8_t *)backend->pixels,
        .size = sizeof backend->pixels,
        .width = 2U,
        .height = 1U,
        .stride = sizeof backend->pixels,
        .bytes_per_pixel = 4U,
        .format = {.bits_per_pixel = 32U},
    };
    return true;
}

/* The host a client hands the socket, over a controller and a store the way mesh_app_init() does.
 */
struct control_test_ui {
    struct mesh_ui_store *store;
    struct mesh_ui_controller *controller;
};

static void control_test_press(void *userdata, enum inkcell_key key) {
    mesh_ui_controller_handle_key(((struct control_test_ui *)userdata)->controller, key);
}

static const char *control_test_screen(void *userdata) {
    return mesh_ui_screen_id(((struct control_test_ui *)userdata)->store->nav.screen);
}

static struct mesh_app_control_host control_test_host(struct control_test_ui *ui) {
    const struct mesh_app_control_host host = {
        .frames = &ui->controller->frames,
        .press = control_test_press,
        .screen = control_test_screen,
        .userdata = ui,
    };
    return host;
}

struct control_test {
    struct control_test_ui ui;
    struct inkwell_loop loop;
    struct mesh_ui_store store;
    struct mesh_ui_controller controller;
    struct mesh_app_control control;
    struct control_test_backend backend;
    struct inkcell_backend vtable;
    char path[64];
    int fd;
};

static bool control_test_open(struct control_test *test, bool with_frame) {
    memset(test, 0, sizeof *test);
    test->fd = -1;
    snprintf(test->path, sizeof test->path, "/tmp/meshclient-test-%ld.sock", (long)getpid());
    test->vtable = (struct inkcell_backend){
        .name = "test-control",
        .present = control_test_present,
        .animating = control_test_moving,
        .frame = with_frame ? control_test_frame : NULL,
    };
    test->backend.pixels[0] = 0xFF112233U;
    test->backend.pixels[1] = 0xFF445566U;
    if (inkwell_loop_init(&test->loop) != 0) {
        return false;
    }
    if (mesh_ui_store_init(&test->store) != 0) {
        inkwell_loop_shutdown(&test->loop);
        return false;
    }
    test->ui = (struct control_test_ui){.store = &test->store, .controller = &test->controller};
    const struct mesh_app_control_host host = control_test_host(&test->ui);
    if (mesh_ui_controller_init(&test->controller, &test->store, &test->vtable, &test->backend,
                                &test->loop) != 0 ||
        mesh_app_control_open(&test->control, &test->loop, &host, test->path) != 0) {
        mesh_ui_controller_shutdown(&test->controller);
        mesh_ui_store_shutdown(&test->store);
        inkwell_loop_shutdown(&test->loop);
        return false;
    }
    /* The first frame, which the controller asks for at init. */
    (void)inkwell_loop_run(&test->loop, 0);

    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path, test->path, strlen(test->path) + 1U);
    test->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    return test->fd >= 0 &&
           connect(test->fd, (const struct sockaddr *)&address, sizeof address) == 0;
}

static void control_test_close(struct control_test *test) {
    if (test->fd >= 0) {
        close(test->fd);
    }
    mesh_app_control_close(&test->control);
    mesh_ui_controller_shutdown(&test->controller);
    mesh_ui_store_shutdown(&test->store);
    inkwell_loop_shutdown(&test->loop);
}

/* Sends `command` and turns the loop until a whole answer is back, or `budget_ms` runs out.
   False on no answer, which some cases want. */
static bool control_test_ask(struct control_test *test, const char *command, char *answer,
                             size_t cap, int budget_ms) {
    if (command != NULL) {
        char line[256];
        const int len = snprintf(line, sizeof line, "%s\n", command);
        if (write(test->fd, line, (size_t)len) != len) {
            return false;
        }
    }
    size_t got = 0U;
    for (int spent = 0; spent < budget_ms; spent += 5) {
        (void)inkwell_loop_run(&test->loop, 5);
        struct pollfd poll_fd = {.fd = test->fd, .events = POLLIN};
        while (poll(&poll_fd, 1, 0) == 1 && got < cap - 1U) {
            const ssize_t n = read(test->fd, answer + got, 1U);
            if (n <= 0) {
                return false;
            }
            if (answer[got] == '\n') {
                answer[got] = '\0';
                return true;
            }
            got += 1U;
        }
    }
    return false;
}

MESH_TEST_CASE(app_control_key_presses_through_the_controller, unit) {
    struct control_test test;
    MESH_TEST_FAIL_IF(!control_test_open(&test, true), "control socket setup failed");

    char answer[256] = {0};
    const char *failure = NULL;
    if (!control_test_ask(&test, "screen", answer, sizeof answer, 1000) ||
        strcmp(answer, "ok messages") != 0) {
        failure = "the client must open on Messages, and say so";
    } else if (!control_test_ask(&test, "key R1", answer, sizeof answer, 1000) ||
               strcmp(answer, "ok") != 0) {
        failure = "a key by name must be answered ok";
    } else if (test.store.nav.screen != MESH_UI_SCREEN_NODES) {
        failure = "...and must move the nav the way the button does";
    } else if (!control_test_ask(&test, "key r1 bogus", answer, sizeof answer, 1000) ||
               strncmp(answer, "error", 5U) != 0) {
        failure = "an unknown key must be answered with an error";
    } else if (test.store.nav.screen != MESH_UI_SCREEN_NODES) {
        failure = "...and must press nothing, not even the good name before it";
    } else if (!control_test_ask(&test, "fly", answer, sizeof answer, 1000) ||
               strncmp(answer, "error", 5U) != 0) {
        failure = "an unknown command must be answered with an error";
    }

    control_test_close(&test);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A picture taken mid-slide is a picture of neither screen, so `shot` holds its answer while the
   backend says it is still moving and takes the frame once it stops. */
MESH_TEST_CASE(app_control_shot_waits_for_the_panel_to_settle, unit) {
    struct control_test test;
    MESH_TEST_FAIL_IF(!control_test_open(&test, true), "control socket setup failed");

    char shot[64];
    snprintf(shot, sizeof shot, "/tmp/meshclient-test-%ld.ppm", (long)getpid());
    char command[128];
    snprintf(command, sizeof command, "shot %s", shot);

    /* Moving from the next frame on, which a refresh asks for. */
    test.backend.moving = true;
    mesh_ui_store_request_refresh(&test.store);

    char answer[256] = {0};
    const char *failure = NULL;
    if (control_test_ask(&test, command, answer, sizeof answer, 300)) {
        failure = "a shot must not be taken while the panel is moving";
    } else {
        test.backend.moving = false;
        char expected[128];
        snprintf(expected, sizeof expected, "ok %s", shot);
        if (!control_test_ask(&test, NULL, answer, sizeof answer, 1000) ||
            strcmp(answer, expected) != 0) {
            failure = "...and must be taken once it stops";
        } else {
            FILE *file = fopen(shot, "rb");
            unsigned char data[32] = {0};
            const size_t len = file != NULL ? fread(data, 1U, sizeof data, file) : 0U;
            if (file != NULL) {
                fclose(file);
            }
            static const unsigned char ppm[] = {'P', '6',  '\n', '2',  ' ',  '1',  '\n', '2', '5',
                                                '5', '\n', 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
            if (len != sizeof ppm || memcmp(data, ppm, len) != 0) {
                failure = "...as the backend's frame, written as a PPM";
            }
        }
    }
    unlink(shot);

    control_test_close(&test);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(app_control_shot_without_a_frame_is_an_error, unit) {
    struct control_test test;
    MESH_TEST_FAIL_IF(!control_test_open(&test, false), "control socket setup failed");

    char answer[256] = {0};
    const bool answered =
        control_test_ask(&test, "shot /tmp/meshclient-test-none.ppm", answer, sizeof answer, 1000);
    control_test_close(&test);
    MESH_TEST_FAIL_IF(!answered || strncmp(answer, "error", 5U) != 0,
                      "a backend with no frame must answer a shot with an error");
    record_success(test_name);
}

/* The path is the caller's choice, and a typo in it must not cost them a file. */
MESH_TEST_CASE(app_control_does_not_replace_a_file_that_is_not_a_socket, unit) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/meshclient-test-file-%ld", (long)getpid());
    const int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    MESH_TEST_FAIL_IF(fd < 0, "could not create the file");
    close(fd);

    struct inkwell_loop loop;
    struct mesh_ui_store store;
    struct mesh_ui_controller controller;
    struct mesh_app_control control;
    MESH_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    MESH_TEST_FAIL_IF(mesh_ui_controller_init(&controller, &store, NULL, NULL, &loop) != 0,
                      "controller init failed");

    struct control_test_ui ui = {.store = &store, .controller = &controller};
    const struct mesh_app_control_host host = control_test_host(&ui);
    const int opened = mesh_app_control_open(&control, &loop, &host, path);
    struct stat info;
    const bool kept = stat(path, &info) == 0 && S_ISREG(info.st_mode);
    unlink(path);
    if (opened == 0) {
        mesh_app_control_close(&control);
    }
    mesh_ui_controller_shutdown(&controller);
    mesh_ui_store_shutdown(&store);
    inkwell_loop_shutdown(&loop);

    MESH_TEST_FAIL_IF(opened >= 0, "a regular file at the path must be refused");
    MESH_TEST_FAIL_IF(!kept, "...and left where it was");
    record_success(test_name);
}

/* Read before any flag or variable could set it, so a launch that asks for no socket gets none
   rather than whatever the stack held. */
MESH_TEST_CASE(app_control_is_off_in_the_default_config, unit) {
    struct mesh_app_config config;
    memset(&config, 0xA5, sizeof config);
    config = mesh_app_config_default();
    MESH_TEST_FAIL_IF(config.ui_control_path[0] != '\0', "the default config must open no socket");
    record_success(test_name);
}

MESH_TEST_CASE(app_control_send_to_nothing_says_so, unit) {
    FILE *sink = fopen("/dev/null", "w");
    MESH_TEST_FAIL_IF(sink == NULL, "no /dev/null");
    const int sent = mesh_app_control_send("/tmp/meshclient-test-absent.sock", "ping", sink);
    fclose(sink);
    MESH_TEST_FAIL_IF(sent >= 0, "a socket that is not there must not be answered ok");
    record_success(test_name);
}
