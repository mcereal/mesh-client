#define _POSIX_C_SOURCE 200809L

/*
 * The control socket, from this client's side of it.
 *
 * The socket is inkstand's (include/inkstand/app/control.h), and its own suite holds what a
 * driver relies on about it: bad names press nothing, a shot waits for the panel to settle, the
 * socket never deletes a file that is not its own. What is left here is the half that is this
 * client's: that the host it hands the socket presses through the controller the way a button
 * does and names the screen the way a scene file does, and that nothing opens one unasked.
 */

#include "framework/mesh_test.h"

#include "inkcell/ui/backend.h"
#include "inkstand/app/control.h"
#include "inkwell/runtime/loop.h"
#include "mesh/core/config.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/route.h"
#include "mesh/ui/store.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void control_test_present(void *state, const void *snapshot, void *userdata) {
    (void)state;
    (void)snapshot;
    (void)userdata;
}

/* The host mesh_app_init() hands the socket, over a controller and a store. */
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

/* Sends `command` and turns the loop until a whole answer is back, or a second runs out. */
static bool control_test_ask(struct inkwell_loop *loop, int fd, const char *command, char *answer,
                             size_t cap) {
    char line[256];
    const int len = snprintf(line, sizeof line, "%s\n", command);
    if (write(fd, line, (size_t)len) != len) {
        return false;
    }
    size_t got = 0U;
    for (int spent = 0; spent < 1000; spent += 5) {
        (void)inkwell_loop_run(loop, 5);
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        while (poll(&poll_fd, 1, 0) == 1 && got < cap - 1U) {
            if (read(fd, answer + got, 1U) != 1) {
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
    struct inkwell_loop loop;
    struct mesh_ui_store store;
    struct mesh_ui_controller controller;
    struct inkstand_control control;
    const struct inkcell_backend backend = {.name = "test-control",
                                            .present = control_test_present};
    char path[64];
    snprintf(path, sizeof path, "/tmp/meshclient-test-%ld.sock", (long)getpid());

    MESH_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_store_init(&store) != 0, inkwell_loop_shutdown(&loop),
                              "store init failed");
    struct control_test_ui ui = {.store = &store, .controller = &controller};
    const struct inkstand_control_host host = {
        .frames = &controller.frames,
        .press = control_test_press,
        .screen = control_test_screen,
        .userdata = &ui,
    };
    const char *failure = NULL;
    bool opened = false;
    int fd = -1;
    if (mesh_ui_controller_init(&controller, &store, &backend, NULL, &loop) != 0) {
        failure = "controller init failed";
    } else if (inkstand_control_open(&control, &loop, &host, path) != 0) {
        failure = "control socket open failed";
    } else {
        opened = true;
        (void)inkwell_loop_run(&loop, 0);
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        memcpy(address.sun_path, path, strlen(path) + 1U);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || connect(fd, (const struct sockaddr *)&address, sizeof address) != 0) {
            failure = "could not connect";
        }
    }

    char answer[256] = {0};
    if (failure == NULL) {
        if (!control_test_ask(&loop, fd, "screen", answer, sizeof answer) ||
            strcmp(answer, "ok messages") != 0) {
            failure = "the client must open on Messages, and say so by its scene id";
        } else if (!control_test_ask(&loop, fd, "key R1", answer, sizeof answer) ||
                   strcmp(answer, "ok") != 0) {
            failure = "a key by name must be answered ok";
        } else if (store.nav.screen != MESH_UI_SCREEN_NODES) {
            failure = "...and must move the nav the way the button does";
        } else if (!control_test_ask(&loop, fd, "screen", answer, sizeof answer) ||
                   strcmp(answer, "ok nodes") != 0) {
            failure = "screen must follow the nav";
        }
    }

    if (fd >= 0) {
        close(fd);
    }
    if (opened) {
        inkstand_control_close(&control);
    }
    mesh_ui_controller_shutdown(&controller);
    mesh_ui_store_shutdown(&store);
    inkwell_loop_shutdown(&loop);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
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
