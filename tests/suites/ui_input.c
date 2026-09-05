#define _POSIX_C_SOURCE 200809L

/* evdev key mapping, the Brick's face buttons, and controller dispatch. */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/core/event_loop.h"
#include "mesh/core/message.h"
#include "mesh/ui/backend.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/stub.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/input.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"

#include <errno.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct test_key_capture {
    enum mesh_ui_key keys[16];
    size_t count;
};

static void test_capture_key(void *userdata, enum mesh_ui_key key) {
    struct test_key_capture *capture = (struct test_key_capture *)userdata;
    if (capture->count < sizeof(capture->keys) / sizeof(capture->keys[0])) {
        capture->keys[capture->count++] = key;
    }
}

struct test_action_capture {
    struct mesh_ui_action last;
    size_t count;
};

static void test_capture_action(void *userdata, const struct mesh_ui_action *action) {
    struct test_action_capture *capture = (struct test_action_capture *)userdata;
    capture->last = *action;
    capture->count++;
}

MESH_TEST_CASE(ui_controller_dispatch, unit) {
    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        return;
    }

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "store init failed");
        return;
    }

    struct mesh_ui_backend_stub_context context;
    memset(&context, 0, sizeof context);

    struct mesh_ui_controller controller;
    if (mesh_ui_controller_init(&controller, &store, mesh_ui_backend_stub(), &context, &loop) !=
        0) {
        mesh_ui_store_shutdown(&store);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "controller init failed");
        return;
    }

    struct mesh_ui_device devices[1] = {
        {.identifier = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -50, .connected = false},
    };
    mesh_ui_store_set_discovery(&store, devices, 1U);
    mesh_event_loop_run(&loop, 0);

    if (!context.has_snapshot || context.present_calls == 0U) {
        mesh_ui_controller_shutdown(&controller);
        mesh_ui_store_shutdown(&store);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "backend did not receive discovery update");
        return;
    }

    if (context.last_snapshot.device_count != 1U ||
        (context.last_snapshot.update_flags & MESH_UI_UPDATE_DISCOVERY) == 0U) {
        mesh_ui_controller_shutdown(&controller);
        mesh_ui_store_shutdown(&store);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "snapshot content mismatch");
        return;
    }

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof(handshake));
    handshake.config_complete = true;
    handshake.config_complete_id = 7U;
    handshake.request_in_flight = false;
    handshake.request_id = 7U;
    handshake.node_count = 2U;
    handshake.has_my_info = false;
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_event_loop_run(&loop, 0);

    if (!context.has_snapshot ||
        (context.last_snapshot.update_flags & MESH_UI_UPDATE_HANDSHAKE) == 0U ||
        !context.last_snapshot.handshake_valid) {
        mesh_ui_controller_shutdown(&controller);
        mesh_ui_store_shutdown(&store);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "backend did not receive handshake update");
        return;
    }

    mesh_ui_controller_shutdown(&controller);
    mesh_ui_store_shutdown(&store);
    mesh_event_loop_shutdown(&loop);
    record_success(test_name);
}

/* The device has no console, so the quit mapping has to be correctable from launch.sh
   without a rebuild. */
MESH_TEST_CASE(ui_input_quit_keys, unit) {
    unsetenv("MESHCLIENT_QUIT_KEYS");
    mesh_ui_input_reload_quit_keys();

    if (!mesh_ui_input_is_quit_key(KEY_MENU) || !mesh_ui_input_is_quit_key(KEY_ESC)) {
        record_failure(test_name, "default quit keys should include MENU and ESC");
        return;
    }

    /* BTN_START stays free for the menu work still to come. */
    MESH_TEST_FAIL_IF(mesh_ui_input_is_quit_key(BTN_START), "BTN_START should not quit by default");

    MESH_TEST_FAIL_IF(mesh_ui_input_quit_hint() == NULL || mesh_ui_input_quit_hint()[0] == '\0',
                      "quit hint should not be empty");

    setenv("MESHCLIENT_QUIT_KEYS", "300, 301", 1);
    mesh_ui_input_reload_quit_keys();

    if (!mesh_ui_input_is_quit_key(300U) || !mesh_ui_input_is_quit_key(301U)) {
        unsetenv("MESHCLIENT_QUIT_KEYS");
        mesh_ui_input_reload_quit_keys();
        record_failure(test_name, "override should install the listed codes");
        return;
    }

    if (mesh_ui_input_is_quit_key(KEY_MENU)) {
        unsetenv("MESHCLIENT_QUIT_KEYS");
        mesh_ui_input_reload_quit_keys();
        record_failure(test_name, "override should replace the defaults, not extend them");
        return;
    }

    /* A garbage override must fall back rather than leave nothing able to quit. */
    setenv("MESHCLIENT_QUIT_KEYS", "not-a-code", 1);
    mesh_ui_input_reload_quit_keys();
    if (!mesh_ui_input_is_quit_key(KEY_MENU)) {
        unsetenv("MESHCLIENT_QUIT_KEYS");
        mesh_ui_input_reload_quit_keys();
        record_failure(test_name, "unparseable override should fall back to defaults");
        return;
    }

    unsetenv("MESHCLIENT_QUIT_KEYS");
    mesh_ui_input_reload_quit_keys();
    record_success(test_name);
}

/* Regression: the transport line used to be printed from inside print_devices(), which only
   runs for MESH_UI_UPDATE_DISCOVERY. A BLE state change that did not also change the device
   list (waiting-for-bluez -> waiting-for-adapter) therefore never reached the console. */
MESH_TEST_CASE(ui_cli_transport_update, unit) {
    const struct mesh_ui_backend *backend = mesh_ui_backend_cli();
    if (backend == NULL || backend->present == NULL) {
        record_failure(test_name, "cli backend unavailable");
        return;
    }

    struct mesh_ui_backend_cli_context context;
    memset(&context, 0, sizeof context);

    /* present() also writes to stderr; tty_stream is the part we can capture. */
    FILE *capture = tmpfile();
    MESH_TEST_FAIL_IF(capture == NULL, "tmpfile failed");
    context.tty_stream = capture;

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.update_flags = MESH_UI_UPDATE_TRANSPORT;
    snprintf(snapshot.transport_status, sizeof snapshot.transport_status, "waiting-for-adapter");

    backend->present(&context, &snapshot, &context);

    fflush(capture);
    rewind(capture);
    char buffer[512];
    const size_t read_bytes = fread(buffer, 1, sizeof(buffer) - 1U, capture);
    buffer[read_bytes] = '\0';
    fclose(capture);

    MESH_TEST_FAIL_IF(strstr(buffer, "waiting-for-adapter") == NULL,
                      "transport-only update should still print the transport line");

    record_success(test_name);
}

MESH_TEST_CASE(ui_input_key_mapping, unit) {
    const char *failure = NULL;
    unsetenv("MESHCLIENT_QUIT_KEYS");
    mesh_ui_input_reload_quit_keys();

    struct mesh_event_loop loop;
    MESH_TEST_FAIL_IF(mesh_event_loop_init(&loop) != 0, "event loop init failed");

    struct test_key_capture capture;
    memset(&capture, 0, sizeof capture);
    struct mesh_ui_input input;
    memset(&input, 0, sizeof input);
    input.loop = &loop; /* not opening /dev/input: only the translation is under test */
    mesh_ui_input_set_handler(&input, test_capture_key, &capture);

    /* The Brick's gamepad: face buttons as BTN_ codes, d-pad as hat axes. */
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_SOUTH, 1);
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_SOUTH, 0); /* release: nothing */
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_EAST, 1);
    mesh_ui_input_handle_event(&input, EV_ABS, ABS_HAT0Y, -1); /* up */
    mesh_ui_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 0);  /* centre: nothing */
    mesh_ui_input_handle_event(&input, EV_ABS, ABS_HAT0X, 1);  /* right */
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_TL, 1);
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_DOWN, 2); /* keyboard autorepeat counts */
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_SELECT, 1);
    mesh_ui_input_handle_event(&input, EV_SYN, 0, 0);
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_F1, 1); /* unmapped: nothing */

    /* BTN_SOUTH is the Brick's B and BTN_EAST its A (Nintendo layout). */
    const enum mesh_ui_key expected[] = {
        MESH_UI_KEY_B,  MESH_UI_KEY_A,    MESH_UI_KEY_UP,     MESH_UI_KEY_RIGHT,
        MESH_UI_KEY_L1, MESH_UI_KEY_DOWN, MESH_UI_KEY_SELECT,
    };
    const size_t expected_count = sizeof(expected) / sizeof(expected[0]);
    if (capture.count != expected_count) {
        failure = "unexpected number of logical keys";
        goto cleanup;
    }
    for (size_t i = 0; i < expected_count; ++i) {
        if (capture.keys[i] != expected[i]) {
            failure = "logical key order mismatch";
            goto cleanup;
        }
    }
    if (loop.stop_requested) {
        failure = "navigation keys must not stop the loop";
        goto cleanup;
    }

    /* MENU (as either device reports it) still quits, and never reaches the handler. */
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_MODE, 1);
    if (!loop.stop_requested || capture.count != expected_count) {
        failure = "MENU should stop the loop without emitting a key";
        goto cleanup;
    }
    if (mesh_ui_input_is_quit_key(BTN_SELECT) || mesh_ui_input_is_quit_key(BTN_START)) {
        failure = "SELECT/START are navigation keys, not quit keys";
        goto cleanup;
    }

cleanup:
    mesh_event_loop_shutdown(&loop);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

MESH_TEST_CASE(ui_controller_key_dispatch, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        return;
    }
    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "store init failed");
        return;
    }

    struct mesh_ui_backend_stub_context backend;
    memset(&backend, 0, sizeof backend);
    struct mesh_ui_controller controller;
    if (mesh_ui_controller_init(&controller, &store, mesh_ui_backend_stub(), &backend, &loop) !=
        0) {
        mesh_ui_store_shutdown(&store);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "controller init failed");
        return;
    }
    struct test_action_capture actions;
    memset(&actions, 0, sizeof actions);
    mesh_ui_controller_set_action_handler(&controller, test_capture_action, &actions);

    mesh_test_nav_populate(&store);
    mesh_event_loop_run(&loop, 0);
    const size_t presents_before = backend.present_calls;

    /* Right lands on Nodes; the repaint arrives through the eventfd on the next turn. */
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_RIGHT);
    mesh_event_loop_run(&loop, 0);
    if (backend.present_calls <= presents_before ||
        backend.last_snapshot.nav.screen != MESH_UI_SCREEN_NODES ||
        (backend.last_snapshot.update_flags & MESH_UI_UPDATE_NAV) == 0U) {
        failure = "key presses should repaint with the new tab";
        goto cleanup;
    }

    /* Back to Messages, open the primary channel, and send its first canned reply: the action
       reaches the handler once. */
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_LEFT);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_DOWN);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_A);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_Y);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_A);
    if (actions.count != 1U || actions.last.type != MESH_UI_ACTION_SEND_TEXT ||
        actions.last.dest != MESH_MESSAGE_BROADCAST_ADDR ||
        strcmp(actions.last.text, mesh_ui_canned_text(0)) != 0) {
        failure = "send action did not reach the handler";
        goto cleanup;
    }

    /* Navigation-only keys never call the handler. */
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_UP);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_NONE);
    if (actions.count != 1U) {
        failure = "navigation keys must not produce actions";
        goto cleanup;
    }

cleanup:
    mesh_ui_controller_shutdown(&controller);
    mesh_ui_store_shutdown(&store);
    mesh_event_loop_shutdown(&loop);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* The app turns a save into a full-section write from the radio's own copy. */
/* The clock push: shaped like a write on the wire, deliberately invisible to the save
   accounting so it never toasts over the user's own save. */
/* The Brick's face buttons do not report by position, and getting this wrong is silent: the
   binding still does something, just the wrong thing. Every code here was read off the device
   log by pressing that button. */
MESH_TEST_CASE(input_brick_face_buttons, unit) {
    static const struct {
        uint16_t code;
        enum mesh_ui_key key;
        const char *printed;
    } k_expected[] = {
        {305U, MESH_UI_KEY_A, "A (right)"},
        {304U, MESH_UI_KEY_B, "B (bottom)"},
        {308U, MESH_UI_KEY_X, "X (top)"},
        {307U, MESH_UI_KEY_Y, "Y (left)"},
    };
    for (size_t i = 0; i < sizeof k_expected / sizeof k_expected[0]; ++i) {
        if (mesh_ui_input_map_key(k_expected[i].code) != k_expected[i].key) {
            char detail[96];
            snprintf(detail, sizeof detail, "code %u is the Brick's %s", k_expected[i].code,
                     k_expected[i].printed);
            record_failure(test_name, detail);
            return;
        }
    }
    record_success(test_name);
}
