#define _POSIX_C_SOURCE 200809L

/* evdev key mapping, the Brick's face buttons, and controller dispatch. */

#include "inkcell/ui/actions.h"
#include "inkcell/ui/backend.h"
#include "inkcell/ui/input.h"
#include "inkcell/ui/input_profile.h"
#include "inkwell/base/array.h"

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "inkwell/runtime/loop.h"
#include "mesh/core/message.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/stub.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/focus.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"

#include "inkcell/ui/input_codes.h"
#include "inkwell/runtime/timer.h"
#include <errno.h>
#include <poll.h>
#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

struct test_key_capture {
    enum inkcell_key keys[16];
    size_t count;
};

static void test_capture_key(void *userdata, enum inkcell_key key) {
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
    struct inkwell_loop loop;
    if (inkwell_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        return;
    }

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "store init failed");
        return;
    }

    struct mesh_ui_backend_stub_context context;
    memset(&context, 0, sizeof context);

    struct mesh_ui_controller controller;
    if (mesh_ui_controller_init(&controller, &store, mesh_ui_backend_stub(), &context, &loop) !=
        0) {
        mesh_ui_store_shutdown(&store);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "controller init failed");
        return;
    }

    struct mesh_ui_device devices[1] = {
        {.identifier = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -50, .connected = false},
    };
    mesh_ui_store_set_discovery(&store, devices, 1U);
    inkwell_loop_run(&loop, 0);

    if (!context.has_snapshot || context.present_calls == 0U) {
        mesh_ui_controller_shutdown(&controller);
        mesh_ui_store_shutdown(&store);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "backend did not receive discovery update");
        return;
    }

    if (context.last_snapshot.device_count != 1U ||
        (context.last_snapshot.update_flags & MESH_UI_UPDATE_DISCOVERY) == 0U) {
        mesh_ui_controller_shutdown(&controller);
        mesh_ui_store_shutdown(&store);
        inkwell_loop_shutdown(&loop);
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
    inkwell_loop_run(&loop, 0);

    if (!context.has_snapshot ||
        (context.last_snapshot.update_flags & MESH_UI_UPDATE_HANDSHAKE) == 0U ||
        !context.last_snapshot.handshake_valid) {
        mesh_ui_controller_shutdown(&controller);
        mesh_ui_store_shutdown(&store);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "backend did not receive handshake update");
        return;
    }

    mesh_ui_controller_shutdown(&controller);
    mesh_ui_store_shutdown(&store);
    inkwell_loop_shutdown(&loop);
    record_success(test_name);
}

/* The device has no console, so the quit mapping has to be correctable from launch.sh
   without a rebuild. */
MESH_TEST_CASE(ui_input_quit_keys, unit) {
    unsetenv("MESHCLIENT_QUIT_KEYS");
    inkcell_input_reload_quit_keys();

    if (!inkcell_input_is_quit_key(KEY_MENU) || !inkcell_input_is_quit_key(KEY_ESC)) {
        record_failure(test_name, "default quit keys should include MENU and ESC");
        return;
    }

    /* BTN_START stays free for the menu work still to come. */
    MESH_TEST_FAIL_IF(inkcell_input_is_quit_key(BTN_START), "BTN_START should not quit by default");

    /* Regression: KEY_POWER used to quit. The Brick's PMIC emits it on a tap of the power
       button, which is this hardware's sleep gesture, so the client died instead of the
       console suspending. Measured on-device with `make deploy-input-map`. */
    MESH_TEST_FAIL_IF(inkcell_input_is_quit_key(KEY_POWER),
                      "KEY_POWER should not quit: it is the Brick's sleep gesture");

    MESH_TEST_FAIL_IF(inkcell_input_quit_hint() == NULL || inkcell_input_quit_hint()[0] == '\0',
                      "quit hint should not be empty");

    setenv("MESHCLIENT_QUIT_KEYS", "300, 301", 1);
    inkcell_input_reload_quit_keys();

    if (!inkcell_input_is_quit_key(300U) || !inkcell_input_is_quit_key(301U)) {
        unsetenv("MESHCLIENT_QUIT_KEYS");
        inkcell_input_reload_quit_keys();
        record_failure(test_name, "override should install the listed codes");
        return;
    }

    if (inkcell_input_is_quit_key(KEY_MENU)) {
        unsetenv("MESHCLIENT_QUIT_KEYS");
        inkcell_input_reload_quit_keys();
        record_failure(test_name, "override should replace the defaults, not extend them");
        return;
    }

    /* A garbage override must fall back rather than leave nothing able to quit. */
    setenv("MESHCLIENT_QUIT_KEYS", "not-a-code", 1);
    inkcell_input_reload_quit_keys();
    if (!inkcell_input_is_quit_key(KEY_MENU)) {
        unsetenv("MESHCLIENT_QUIT_KEYS");
        inkcell_input_reload_quit_keys();
        record_failure(test_name, "unparseable override should fall back to defaults");
        return;
    }

    unsetenv("MESHCLIENT_QUIT_KEYS");
    inkcell_input_reload_quit_keys();
    record_success(test_name);
}

/* Regression: the transport line used to be printed from inside print_devices(), which only
   runs for MESH_UI_UPDATE_DISCOVERY. A BLE state change that did not also change the device
   list (waiting-for-bluez -> waiting-for-adapter) therefore never reached the console. */
MESH_TEST_CASE(ui_cli_transport_update, unit) {
    const struct inkcell_backend *backend = mesh_ui_backend_cli();
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

/* What a quit key does, for a case that is not running a loop: inkcell asks the host to stop
   and the host here is one event loop the case owns. */
static void test_request_stop(void *ctx) { inkwell_loop_request_stop((struct inkwell_loop *)ctx); }

MESH_TEST_CASE(ui_input_key_mapping, unit) {
    const char *failure = NULL;
    unsetenv("MESHCLIENT_QUIT_KEYS");
    inkcell_input_reload_quit_keys();

    struct inkwell_loop loop;
    MESH_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "event loop init failed");

    struct test_key_capture capture;
    memset(&capture, 0, sizeof capture);
    struct inkcell_input input;
    memset(&input, 0, sizeof input);
    /* Not opening /dev/input: only the translation is under test, so the host vtable is the
       stop callback and nothing else. */
    input.host.ctx = &loop;
    input.host.request_stop = test_request_stop;
    inkcell_input_set_handler(&input, test_capture_key, &capture);

    /* The Brick's gamepad: face buttons as BTN_ codes, d-pad as hat axes. */
    inkcell_input_handle_event(&input, EV_KEY, BTN_SOUTH, 1);
    inkcell_input_handle_event(&input, EV_KEY, BTN_SOUTH, 0); /* release: nothing */
    inkcell_input_handle_event(&input, EV_KEY, BTN_EAST, 1);
    inkcell_input_handle_event(&input, EV_ABS, ABS_HAT0Y, -1); /* up */
    inkcell_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 0);  /* centre: nothing */
    inkcell_input_handle_event(&input, EV_ABS, ABS_HAT0X, 1);  /* right */
    inkcell_input_handle_event(&input, EV_KEY, BTN_TL, 1);
    /* A direction's kernel autorepeat is dropped - our own timer drives those, and honouring
       both would take two rows per step. A face button's still counts. */
    inkcell_input_handle_event(&input, EV_KEY, KEY_DOWN, 2);
    inkcell_input_handle_event(&input, EV_KEY, KEY_ENTER, 2);
    inkcell_input_handle_event(&input, EV_KEY, BTN_SELECT, 1);
    inkcell_input_handle_event(&input, EV_SYN, 0, 0);
    inkcell_input_handle_event(&input, EV_KEY, KEY_F12, 1); /* unmapped: nothing */

    /* BTN_SOUTH is the Brick's B and BTN_EAST its A (Nintendo layout). */
    const enum inkcell_key expected[] = {
        INKCELL_KEY_B,  INKCELL_KEY_A, INKCELL_KEY_UP,     INKCELL_KEY_RIGHT,
        INKCELL_KEY_L1, INKCELL_KEY_A, INKCELL_KEY_SELECT,
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
    inkcell_input_handle_event(&input, EV_KEY, BTN_MODE, 1);
    if (!loop.stop_requested || capture.count != expected_count) {
        failure = "MENU should stop the loop without emitting a key";
        goto cleanup;
    }
    if (inkcell_input_is_quit_key(BTN_SELECT) || inkcell_input_is_quit_key(BTN_START)) {
        failure = "SELECT/START are navigation keys, not quit keys";
        goto cleanup;
    }

    /*
     * The three caps a keyboard can reach now, and could not before.
     *
     * They are checked here because this client is what needs them: nav.c binds all three -
     * START resends a message, SELECT opens the help sheet, X is the messages screen's verb -
     * so before this, a run driven from a keyboard could walk every screen and reach none of
     * those. They are pure mapping questions, so they are asked of the mapper rather than
     * pushed through the handler like the sequence above.
     */
    if (inkcell_input_map_key(KEY_X) != INKCELL_KEY_X ||
        inkcell_input_map_key(KEY_F1) != INKCELL_KEY_START ||
        inkcell_input_map_key(KEY_F2) != INKCELL_KEY_SELECT) {
        failure = "a keyboard must be able to reach X, START and SELECT";
        goto cleanup;
    }

cleanup:
    inkwell_loop_shutdown(&loop);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * L2 and R2, which are not buttons on this hardware.
 *
 * The Brick's pad impersonates an Xbox 360 controller and a 360's triggers are analogue: there
 * is no BTN_TL2 in its key bitmap at all, so anything that went looking for one found nothing
 * and the two buttons on the case went unread. They are ABS_Z and ABS_RZ, and this is the
 * mapping that reads them.
 *
 * The edge is the assertion worth making. On this pad a trigger is digital - 255 down, 0 up -
 * but it is declared as an axis, and a pad that reports the way up as a run of rising values
 * would be one press of shift per value on the way to a single squeeze. It is also the reason
 * the hat's own "value 0 is the release" cannot simply be reused: a trigger at rest and a d-pad
 * at centre report the same 0, and only one of them is a key coming back up.
 */
MESH_TEST_CASE(input_triggers_are_axes_and_press_on_the_edge, unit) {
    const char *failure = NULL;
    unsetenv("MESHCLIENT_QUIT_KEYS");
    inkcell_input_reload_quit_keys();

    /* The pure mapping first, which is what the profile's codes are asserted by number for. */
    MESH_TEST_FAIL_IF(inkcell_input_map_trigger(ABS_Z, 255) != INKCELL_KEY_L2, "ABS_Z is L2");
    MESH_TEST_FAIL_IF(inkcell_input_map_trigger(ABS_RZ, 255) != INKCELL_KEY_R2, "ABS_RZ is R2");
    MESH_TEST_FAIL_IF(inkcell_input_map_trigger(ABS_Z, 0) != INKCELL_KEY_NONE,
                      "a trigger at rest is not a press");
    MESH_TEST_FAIL_IF(inkcell_input_map_trigger(ABS_HAT0X, 255) != INKCELL_KEY_NONE,
                      "the hat is not a trigger");
    MESH_TEST_FAIL_IF(inkcell_input_map_key(ABS_Z) != INKCELL_KEY_NONE,
                      "a trigger must not be read as a key code as well");

    struct test_key_capture capture;
    memset(&capture, 0, sizeof capture);
    struct inkcell_input input;
    memset(&input, 0, sizeof input);
    inkcell_input_set_handler(&input, test_capture_key, &capture);

    /* One squeeze reported as a ramp is one press. */
    inkcell_input_handle_event(&input, EV_ABS, ABS_Z, 40);
    if (capture.count != 0U) {
        failure = "a trigger below the press threshold is not a press";
        goto cleanup;
    }
    inkcell_input_handle_event(&input, EV_ABS, ABS_Z, 200);
    inkcell_input_handle_event(&input, EV_ABS, ABS_Z, 255);
    if (capture.count != 1U || capture.keys[0] != INKCELL_KEY_L2) {
        failure = "a held trigger should emit once, not once per value";
        goto cleanup;
    }

    /* Let go and squeeze again: a second press. */
    inkcell_input_handle_event(&input, EV_ABS, ABS_Z, 0);
    inkcell_input_handle_event(&input, EV_ABS, ABS_Z, 255);
    if (capture.count != 2U || capture.keys[1] != INKCELL_KEY_L2) {
        failure = "releasing and squeezing again is a second press";
        goto cleanup;
    }

    /* The two latch separately, and neither is a direction, so neither arms the hold repeat. */
    inkcell_input_handle_event(&input, EV_ABS, ABS_RZ, 255);
    if (capture.count != 3U || capture.keys[2] != INKCELL_KEY_R2) {
        failure = "R2 has a latch of its own";
        goto cleanup;
    }
    if (inkcell_input_repeat_key(&input) != INKCELL_KEY_NONE) {
        failure = "a trigger is not a direction and must not repeat on hold";
        goto cleanup;
    }

    /* And the hat still works beside them, which is the regression the shared EV_ABS branch
       would otherwise be. */
    inkcell_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 1);
    if (capture.count != 4U || capture.keys[3] != INKCELL_KEY_DOWN) {
        failure = "the hat should still report through the same branch";
        goto cleanup;
    }

cleanup:
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Holding the d-pad has to scroll. The Brick reports the d-pad as ABS_HAT0X/Y, and an absolute
 * axis gets no kernel autorepeat however long it is held - one event out of centre, one back -
 * so walking a 60-node roster used to be 60 presses. The repeat is generated in the input layer
 * instead. No timerfd here: a zeroed struct has none, and the hold is stepped by hand. */
MESH_TEST_CASE(ui_input_key_repeat, unit) {
    const char *failure = NULL;
    unsetenv("MESHCLIENT_QUIT_KEYS");
    unsetenv("MESHCLIENT_KEY_REPEAT_DELAY_MS");
    unsetenv("MESHCLIENT_KEY_REPEAT_MS");
    inkcell_input_reload_quit_keys();
    inkcell_input_reload_key_repeat();

    /* The first repeat waits out the hold delay; later ones come at the scroll interval, and
       the interval ramps down once the hold is clearly deliberate. */
    const unsigned int hold = inkcell_input_repeat_delay_ms(0U);
    const unsigned int step = inkcell_input_repeat_delay_ms(1U);
    const unsigned int fast = inkcell_input_repeat_delay_ms(64U);
    MESH_TEST_FAIL_IF(hold == 0U || step == 0U || fast == 0U, "repeat should be on by default");
    MESH_TEST_FAIL_IF(hold <= step, "the hold delay must be longer than the scroll interval");
    MESH_TEST_FAIL_IF(fast > step, "a long hold must not scroll slower than a short one");

    struct test_key_capture capture;
    memset(&capture, 0, sizeof capture);
    struct inkcell_input input;
    memset(&input, 0, sizeof input);
    inkcell_input_set_handler(&input, test_capture_key, &capture);

    /* Down on the hat: one row now, and the hold that will produce the rest. */
    inkcell_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 1);
    if (capture.count != 1U || capture.keys[0] != INKCELL_KEY_DOWN ||
        inkcell_input_repeat_key(&input) != INKCELL_KEY_DOWN) {
        failure = "a held direction should emit once and arm the repeat";
        goto cleanup;
    }

    inkcell_input_repeat_tick(&input);
    inkcell_input_repeat_tick(&input);
    if (capture.count != 3U || capture.keys[1] != INKCELL_KEY_DOWN ||
        capture.keys[2] != INKCELL_KEY_DOWN) {
        failure = "each repeat should emit the held direction again";
        goto cleanup;
    }

    /* Centre is the release, and nothing repeats after it. */
    inkcell_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 0);
    inkcell_input_repeat_tick(&input);
    if (capture.count != 3U || inkcell_input_repeat_key(&input) != INKCELL_KEY_NONE) {
        failure = "centring the hat should end the repeat";
        goto cleanup;
    }

    /* A keyboard's arrow key repeats through the same path, and the kernel's own autorepeat is
       dropped rather than counted twice - a direction is driven by our timer or by nothing. */
    inkcell_input_handle_event(&input, EV_KEY, KEY_DOWN, 1);
    inkcell_input_handle_event(&input, EV_KEY, KEY_DOWN, 2);
    if (capture.count != 4U || inkcell_input_repeat_key(&input) != INKCELL_KEY_DOWN) {
        failure = "kernel autorepeat must not double a repeat we are already driving";
        goto cleanup;
    }
    inkcell_input_handle_event(&input, EV_KEY, KEY_DOWN, 0);
    if (inkcell_input_repeat_key(&input) != INKCELL_KEY_NONE) {
        failure = "releasing the key should end the repeat";
        goto cleanup;
    }

    /* A device unplugged mid-hold never sends the release, so losing its fd has to end the hold
       - otherwise the timer scrolls the list until some other button is pressed. */
    inkcell_input_handle_device_event(&input, 7, EV_KEY, KEY_DOWN, 1);
    inkcell_input_device_lost(&input, 9);
    if (inkcell_input_repeat_key(&input) != INKCELL_KEY_DOWN) {
        failure = "another device going away must not end this hold";
        goto cleanup;
    }
    inkcell_input_device_lost(&input, 7);
    if (inkcell_input_repeat_key(&input) != INKCELL_KEY_NONE) {
        failure = "losing the device that started a hold should end it";
        goto cleanup;
    }

    /* Confirm and back never repeat: a held A that fired forty times would open forty things.
       Pressing one also ends a direction still being held. */
    inkcell_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 1);
    inkcell_input_handle_event(&input, EV_KEY, BTN_EAST, 1);
    if (inkcell_input_repeat_key(&input) != INKCELL_KEY_NONE) {
        failure = "A should not repeat, and should stop the direction that was held";
        goto cleanup;
    }

    /* The knobs exist because the device has no console; 0 restores one row per press. Off has
       to mean off on a keyboard too, so the kernel's autorepeat stays dropped. */
    setenv("MESHCLIENT_KEY_REPEAT_DELAY_MS", "0", 1);
    inkcell_input_reload_key_repeat();
    inkcell_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 1);
    if (inkcell_input_repeat_delay_ms(0U) != 0U ||
        inkcell_input_repeat_key(&input) != INKCELL_KEY_NONE) {
        failure = "a zero delay should switch hold-to-scroll off";
        goto cleanup;
    }

    const size_t before_hold = capture.count;
    inkcell_input_handle_event(&input, EV_KEY, KEY_DOWN, 1);
    inkcell_input_handle_event(&input, EV_KEY, KEY_DOWN, 2);
    inkcell_input_handle_event(&input, EV_KEY, KEY_DOWN, 2);
    if (capture.count != before_hold + 1U) {
        failure = "with repeat off, holding a key should still move exactly one row";
        goto cleanup;
    }

cleanup:
    unsetenv("MESHCLIENT_KEY_REPEAT_DELAY_MS");
    unsetenv("MESHCLIENT_KEY_REPEAT_MS");
    inkcell_input_reload_key_repeat();
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ui_host_text_uses_visible_keyboard_and_field_cap, unit) {
    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    store.pending_flags = MESH_UI_UPDATE_NONE;
    const char *failure = NULL;
    if (mesh_ui_store_insert_text(&store, "outside") ||
        store.pending_flags != MESH_UI_UPDATE_NONE) {
        failure = "host text must be ignored outside the keyboard";
        goto cleanup_text;
    }

    store.nav.keyboard_open = true;
    store.nav.keyboard_passkey = true;
    if (!mesh_ui_store_insert_text(&store, "1234") || strcmp(store.nav.draft, "1234") != 0 ||
        (store.pending_flags & MESH_UI_UPDATE_NAV) == 0U) {
        failure = "host text should update the same draft and publish a frame";
        goto cleanup_text;
    }
    store.pending_flags = MESH_UI_UPDATE_NONE;
    store.nav.context_open = true;
    if (!mesh_ui_store_insert_text(&store, "5") || store.nav.context_open ||
        strcmp(store.nav.draft, "1234") != 0 || (store.pending_flags & MESH_UI_UPDATE_NAV) == 0U) {
        failure = "host text should dismiss a context menu without editing behind it";
        goto cleanup_text;
    }
    store.pending_flags = MESH_UI_UPDATE_NONE;
    store.nav.help_open = true;
    if (mesh_ui_store_insert_text(&store, "5") || strcmp(store.nav.draft, "1234") != 0) {
        failure = "text must not reach a keyboard under a help sheet";
        goto cleanup_text;
    }
    store.nav.help_open = false;
    if (!mesh_ui_store_insert_text(&store, "56") || mesh_ui_store_insert_text(&store, "7") ||
        strcmp(store.nav.draft, "123456") != 0) {
        failure = "a passkey must keep its six-byte cap for host typing";
    }

cleanup_text:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ui_controller_key_dispatch, unit) {
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct inkwell_loop loop;
    if (inkwell_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        return;
    }
    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "store init failed");
        return;
    }

    struct mesh_ui_backend_stub_context backend;
    memset(&backend, 0, sizeof backend);
    struct mesh_ui_controller controller;
    if (mesh_ui_controller_init(&controller, &store, mesh_ui_backend_stub(), &backend, &loop) !=
        0) {
        mesh_ui_store_shutdown(&store);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "controller init failed");
        return;
    }
    struct test_action_capture actions;
    memset(&actions, 0, sizeof actions);
    mesh_ui_controller_set_action_handler(&controller, test_capture_action, &actions);

    mesh_test_nav_populate(&store);
    inkwell_loop_run(&loop, 0);
    mesh_ui_controller_handle_shortcut(&controller, 's');
    mesh_ui_controller_handle_shortcut(&controller, 'x');
    if (store.pending_flags != MESH_UI_UPDATE_NONE) {
        failure = "unoffered and unknown shortcuts must not change the current screen";
        goto cleanup;
    }
    mesh_ui_controller_handle_shortcut(&controller, 'n');
    if (!store.nav.picker_open) {
        failure = "New shortcut should open the message recipient picker";
        goto cleanup;
    }
    inkwell_loop_run(&loop, 0);
    mesh_ui_controller_handle_key(&controller, INKCELL_KEY_B);
    inkwell_loop_run(&loop, 0);
    const size_t presents_before = backend.present_calls;

    /*
     * An SDL action hint starts as the logical keycap inkcell drew, then resolves back to the
     * semantic command and the half of its pair. A second hint against the now-stale frame is
     * discarded until the first command has been presented.
     */
    mesh_ui_controller_handle_command(&controller, MESH_UI_COMMAND_TABS,
                                      MESH_UI_COMMAND_DIRECTION_NONE);
    mesh_ui_controller_handle_action_key(&controller, INKCELL_KEY_R1);
    mesh_ui_controller_handle_action_key(&controller, INKCELL_KEY_L1);
    inkwell_loop_run(&loop, 0);
    if (backend.present_calls <= presents_before ||
        backend.last_snapshot.nav.screen != MESH_UI_SCREEN_NODES ||
        (backend.last_snapshot.update_flags & MESH_UI_UPDATE_NAV) == 0U) {
        failure = "semantic tab commands should repaint once with the requested direction";
        goto cleanup;
    }

    /* Commands absent from the presented frame do nothing. */
    mesh_ui_controller_handle_command(&controller, MESH_UI_COMMAND_DELETE,
                                      MESH_UI_COMMAND_DIRECTION_NONE);
    if (store.pending_flags != MESH_UI_UPDATE_NONE) {
        failure = "a command not offered by the frame should be ignored";
        goto cleanup;
    }

    /*
     * Back to Messages, open the primary channel semantically, and send its first canned reply.
     * OPEN has one binding, so adapters do not have to manufacture a direction for it.
     */
    mesh_ui_controller_handle_action_key(&controller, INKCELL_KEY_L1);
    inkwell_loop_run(&loop, 0);
    mesh_ui_controller_handle_key(&controller, INKCELL_KEY_DOWN);
    inkwell_loop_run(&loop, 0);
    mesh_ui_controller_handle_action_key(&controller, INKCELL_KEY_A);
    inkwell_loop_run(&loop, 0);
    mesh_ui_controller_handle_key(&controller, INKCELL_KEY_A);
    mesh_ui_controller_handle_key(&controller, INKCELL_KEY_A);
    if (actions.count != 1U || actions.last.type != MESH_UI_ACTION_SEND_TEXT ||
        actions.last.dest != MESH_MESSAGE_BROADCAST_ADDR ||
        strcmp(actions.last.text, mesh_ui_canned_text(0)) != 0) {
        failure = "send action did not reach the handler";
        goto cleanup;
    }

    /* Navigation-only keys never call the handler. */
    mesh_ui_controller_handle_key(&controller, INKCELL_KEY_UP);
    mesh_ui_controller_handle_key(&controller, INKCELL_KEY_NONE);
    if (actions.count != 1U) {
        failure = "navigation keys must not produce actions";
        goto cleanup;
    }

    /*
     * Quit is the one offered command with no logical key: the input host normally consumes its
     * physical binding, so semantic dispatch stops the controller's loop directly.
     */
    controller.snapshot.nav.screen = MESH_UI_SCREEN_RADIO;
    controller.snapshot.device_count = 1U;
    controller.snapshot.devices[0].connected = true;
    store.pending_flags = MESH_UI_UPDATE_NONE;
    loop.stop_requested = false;
    mesh_ui_controller_handle_command(&controller, MESH_UI_COMMAND_QUIT,
                                      MESH_UI_COMMAND_DIRECTION_NONE);
    if (!loop.stop_requested) {
        failure = "the semantic quit command should stop the UI loop";
        goto cleanup;
    }

cleanup:
    mesh_ui_controller_shutdown(&controller);
    mesh_ui_store_shutdown(&store);
    inkwell_loop_shutdown(&loop);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ui_controller_context_menu_dispatches_semantic_commands, unit) {
    const char *failure = NULL;
    struct inkwell_loop loop;
    if (inkwell_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        return;
    }
    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "store init failed");
        return;
    }
    struct mesh_ui_backend_stub_context backend;
    memset(&backend, 0, sizeof backend);
    struct mesh_ui_controller controller;
    if (mesh_ui_controller_init(&controller, &store, mesh_ui_backend_stub(), &backend, &loop) !=
        0) {
        mesh_ui_store_shutdown(&store);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "controller init failed");
        return;
    }
    struct test_action_capture actions;
    memset(&actions, 0, sizeof actions);
    mesh_ui_controller_set_action_handler(&controller, test_capture_action, &actions);

    mesh_test_nav_populate(&store);
    if (!mesh_test_open_devices(&store)) {
        failure = "the test needs the Devices tab";
        goto cleanup;
    }
    inkwell_loop_run(&loop, 0);
    if (!mesh_ui_store_handle_context(&store, (uint32_t)MESH_UI_FOCUS_ROWS + 1U, 40, 40)) {
        failure = "the second device should open a context menu";
        goto cleanup;
    }
    inkwell_loop_run(&loop, 0);
    if (!controller.snapshot.nav.context_open) {
        failure = "the controller should present the open context menu";
        goto cleanup;
    }

    mesh_ui_controller_handle_click(&controller, (uint32_t)MESH_UI_FOCUS_MENU +
                                                     (uint32_t)MESH_UI_COMMAND_CONNECT);
    if (store.nav.context_open || actions.count != 1U ||
        actions.last.type != MESH_UI_ACTION_CONNECT ||
        strcmp(actions.last.identifier, "AA:BB:CC:DD:EE:02") != 0) {
        failure = "a context verb should invoke its semantic command on the selected row";
        goto cleanup;
    }
    inkwell_loop_run(&loop, 0);
    if (backend.last_snapshot.nav.context_open) {
        failure = "a chosen context command should repaint with the menu dismissed";
        goto cleanup;
    }

    /* The former button-shaped target is not an alias. An unoffered command only dismisses the
       menu and must not repeat the Connect action. */
    if (!mesh_ui_store_handle_context(&store, (uint32_t)MESH_UI_FOCUS_ROWS + 1U, 40, 40)) {
        failure = "the context menu should reopen";
        goto cleanup;
    }
    inkwell_loop_run(&loop, 0);
    mesh_ui_controller_handle_click(&controller,
                                    (uint32_t)MESH_UI_FOCUS_MENU + (uint32_t)INKCELL_BUTTON_A);
    if (store.nav.context_open || actions.count != 1U) {
        failure = "a physical button target should dismiss without dispatching a command";
        goto cleanup;
    }

cleanup:
    mesh_ui_controller_shutdown(&controller);
    mesh_ui_store_shutdown(&store);
    inkwell_loop_shutdown(&loop);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A verb in the heading is its command, the way a verb in the right-click menu is - but answered
 * whenever it is drawn rather than only while a menu is open, and only for a command the screen
 * offers and the heading could have drawn. A stale or forged id presses nothing.
 */
MESH_TEST_CASE(ui_controller_heading_verbs_dispatch_semantic_commands, unit) {
    const char *failure = NULL;
    struct inkwell_loop loop;
    if (inkwell_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        return;
    }
    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "store init failed");
        return;
    }
    struct mesh_ui_backend_stub_context backend;
    memset(&backend, 0, sizeof backend);
    struct mesh_ui_controller controller;
    if (mesh_ui_controller_init(&controller, &store, mesh_ui_backend_stub(), &backend, &loop) !=
        0) {
        mesh_ui_store_shutdown(&store);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "controller init failed");
        return;
    }
    struct test_action_capture actions;
    memset(&actions, 0, sizeof actions);
    mesh_ui_controller_set_action_handler(&controller, test_capture_action, &actions);

    mesh_test_nav_populate(&store);
    if (!mesh_test_open_devices(&store)) {
        failure = "the test needs the Devices tab";
        goto cleanup;
    }
    /* NodeTwo, which is not connected, so the row's verb is Connect. */
    struct mesh_ui_action ignored;
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &ignored);
    inkwell_loop_run(&loop, 0);

    /* Not offered here, and not a heading verb at all: neither presses anything. */
    mesh_ui_controller_handle_click(&controller,
                                    (uint32_t)MESH_UI_FOCUS_BAR + (uint32_t)MESH_UI_COMMAND_REPLY);
    mesh_ui_controller_handle_click(&controller,
                                    (uint32_t)MESH_UI_FOCUS_BAR + (uint32_t)MESH_UI_COMMAND_MOVE);
    if (actions.count != 0U) {
        failure = "a heading id for a verb this screen does not offer should press nothing";
        goto cleanup;
    }

    mesh_ui_controller_handle_click(&controller, (uint32_t)MESH_UI_FOCUS_BAR +
                                                     (uint32_t)MESH_UI_COMMAND_CONNECT);
    if (actions.count != 1U || actions.last.type != MESH_UI_ACTION_CONNECT ||
        strcmp(actions.last.identifier, "AA:BB:CC:DD:EE:02") != 0) {
        failure = "a heading verb should invoke its command on the row the cursor is on";
        goto cleanup;
    }

cleanup:
    mesh_ui_controller_shutdown(&controller);
    mesh_ui_store_shutdown(&store);
    inkwell_loop_shutdown(&loop);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
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
        enum inkcell_key key;
        const char *printed;
    } k_expected[] = {
        {305U, INKCELL_KEY_A, "A (right)"},
        {304U, INKCELL_KEY_B, "B (bottom)"},
        {308U, INKCELL_KEY_X, "X (top)"},
        {307U, INKCELL_KEY_Y, "Y (left)"},
    };
    for (size_t i = 0; i < sizeof k_expected / sizeof k_expected[0]; ++i) {
        if (inkcell_input_map_key(k_expected[i].code) != k_expected[i].key) {
            char detail[96];
            snprintf(detail, sizeof detail, "code %u is the Brick's %s", k_expected[i].code,
                     k_expected[i].printed);
            record_failure(test_name, detail);
            return;
        }
    }
    record_success(test_name);
}

/*
 * The registry has to hold its shape, for the reason the theme table does: anyone may add a row
 * and nothing else in the build would notice a row that bound three face buttons.
 */
MESH_TEST_CASE(input_profiles_are_complete, unit) {
    const size_t count = inkcell_input_profile_count();
    MESH_TEST_FAIL_IF(count == 0U, "the profile registry is empty");
    MESH_TEST_FAIL_IF(inkcell_input_profile_default() == NULL, "there is no default profile");

    for (size_t i = 0; i < count; ++i) {
        const struct inkcell_input_profile *profile = inkcell_input_profile_at(i);
        char reason[128];
        if (profile == NULL) {
            record_failure(test_name, "the registry has a hole in it");
            return;
        }
        if (!inkcell_input_profile_validate(profile, reason, sizeof reason)) {
            record_failure(test_name, reason);
            return;
        }
        /* Two rows with one name is a profile nobody can select, since the lookup takes the
           first match and says nothing about the second. */
        for (size_t j = i + 1U; j < count; ++j) {
            const struct inkcell_input_profile *other = inkcell_input_profile_at(j);
            if (other != NULL && strcasecmp(profile->name, other->name) == 0) {
                char detail[96];
                snprintf(detail, sizeof detail, "two profiles are called %s", profile->name);
                record_failure(test_name, detail);
                return;
            }
        }
    }

    /* The pak ships for the Brick, so that is what an unconfigured client must be. */
    MESH_TEST_FAIL_IF(strcmp(inkcell_input_profile_default()->name, "brick") != 0,
                      "the default profile should be the Brick's");
    record_success(test_name);
}

/*
 * The two conventions disagree about exactly the two buttons that confirm and go back, which is
 * the whole reason a profile exists rather than a longer switch: on a pad following the Xbox
 * convention the code the Brick calls A is B, so a port that extended the table instead of
 * replacing it would swap confirm and back and nothing would fail to compile.
 */
MESH_TEST_CASE(input_profile_decides_which_button_confirms, unit) {
    const struct inkcell_input_profile *brick = inkcell_input_profile_by_name("brick");
    const struct inkcell_input_profile *xbox = inkcell_input_profile_by_name("xbox");
    MESH_TEST_FAIL_IF(brick == NULL || xbox == NULL, "both shipped profiles should resolve");

    MESH_TEST_FAIL_IF(inkcell_input_profile_key(brick, BTN_EAST) != INKCELL_KEY_A,
                      "the Brick's A is BTN_EAST");
    MESH_TEST_FAIL_IF(inkcell_input_profile_key(brick, BTN_SOUTH) != INKCELL_KEY_B,
                      "the Brick's B is BTN_SOUTH");
    MESH_TEST_FAIL_IF(inkcell_input_profile_key(brick, BTN_WEST) != INKCELL_KEY_X,
                      "the Brick's X, on top, is BTN_WEST");
    MESH_TEST_FAIL_IF(inkcell_input_profile_key(brick, BTN_NORTH) != INKCELL_KEY_Y,
                      "the Brick's Y, on the left, is BTN_NORTH");
    MESH_TEST_FAIL_IF(inkcell_input_profile_key(xbox, BTN_SOUTH) != INKCELL_KEY_A,
                      "an Xbox-convention A is BTN_SOUTH");
    MESH_TEST_FAIL_IF(inkcell_input_profile_key(xbox, BTN_EAST) != INKCELL_KEY_B,
                      "an Xbox-convention B is BTN_EAST");

    /*
     * X and Y, which is where this went wrong first and would go wrong again.
     *
     * The compass aliases do not describe an Xbox pad's X and Y: BTN_X *is* BTN_NORTH (307) and
     * BTN_Y *is* BTN_WEST (308), while the buttons carrying those letters are on the left and on
     * top respectively. So the table is written with BTN_X/BTN_Y and asserted against the
     * numbers, because a test written in the same misleading names as the bug would pass it.
     */
    MESH_TEST_FAIL_IF(inkcell_input_profile_key(xbox, 307U) != INKCELL_KEY_X,
                      "an Xbox-convention X (on the left) is 307, BTN_X");
    MESH_TEST_FAIL_IF(inkcell_input_profile_key(xbox, 308U) != INKCELL_KEY_Y,
                      "an Xbox-convention Y (on top) is 308, BTN_Y");

    /*
     * The Brick and an Xbox pad report the *same* two codes for their top and left buttons -
     * both are xpad underneath - and disagree only about which letter is printed there. So X and
     * Y are swapped between the profiles exactly as A and B are, and a profile that shared one
     * pair with the other would be describing one of the two devices wrongly.
     */
    MESH_TEST_FAIL_IF(inkcell_input_profile_key(brick, 307U) ==
                          inkcell_input_profile_key(xbox, 307U),
                      "the two conventions disagree about the left-hand button");
    MESH_TEST_FAIL_IF(inkcell_input_profile_key(brick, 308U) ==
                          inkcell_input_profile_key(xbox, 308U),
                      "the two conventions disagree about the top button");

    /* Name resolution is how a launch.sh sets this, so it takes the spelling a person types. */
    MESH_TEST_FAIL_IF(inkcell_input_profile_by_name("XBOX") != xbox,
                      "profile names should be case-insensitive");
    MESH_TEST_FAIL_IF(inkcell_input_profile_by_name("") != NULL ||
                          inkcell_input_profile_by_name(NULL) != NULL ||
                          inkcell_input_profile_by_name("no-such-pad") != NULL,
                      "an unknown name should not resolve");
    record_success(test_name);
}

/*
 * The regression this whole table exists for.
 *
 * The codes lived in the input layer and the caps in src/ui/tables/actions.c, and a port that
 * corrected one and not the other leaves the action bar naming a key that does something else -
 * which is invisible, because the binding still works. Both public entry points are asked here,
 * under a profile that is not the default, so the two cannot come from different places again.
 */
MESH_TEST_CASE(input_profile_caps_and_codes_move_together, unit) {
    const char *failure = NULL;

    setenv("MESHCLIENT_INPUT_PROFILE", "xbox", 1);
    inkcell_input_profile_reload();

    if (inkcell_input_map_key(BTN_SOUTH) != INKCELL_KEY_A ||
        inkcell_input_map_key(BTN_EAST) != INKCELL_KEY_B) {
        failure = "the selected profile should decide what the face buttons report";
        goto restore;
    }
    if (strcmp(inkcell_button_cap(INKCELL_BUTTON_A), "A") != 0 ||
        strcmp(inkcell_button_cap(INKCELL_BUTTON_B), "B") != 0) {
        failure = "the cap should come from the profile the codes came from";
        goto restore;
    }
    /* A convention is not a profile's to restate, and must survive the switch. */
    if (inkcell_input_map_key(KEY_ENTER) != INKCELL_KEY_A ||
        inkcell_input_map_key(BTN_TL) != INKCELL_KEY_L1) {
        failure = "the shared conventions should answer under any profile";
        goto restore;
    }

    /* A typo is a wrong label, not a client that cannot be driven: it falls back rather than
       leaving the face buttons bound to nothing. */
    setenv("MESHCLIENT_INPUT_PROFILE", "no-such-pad", 1);
    inkcell_input_profile_reload();
    if (inkcell_input_map_key(BTN_EAST) != INKCELL_KEY_A) {
        failure = "an unknown profile should fall back to the default";
    }

restore:
    unsetenv("MESHCLIENT_INPUT_PROFILE");
    inkcell_input_profile_reload();
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    /* And back where every other test in this binary expects to find it. */
    MESH_TEST_FAIL_IF(inkcell_input_map_key(BTN_EAST) != INKCELL_KEY_A,
                      "the unconfigured client should be the Brick");
    record_success(test_name);
}

/* An EVIOCGBIT bitmap with one code set, which is how a node says what it can report. */
struct test_evdev_bits {
    unsigned long words[INKCELL_INPUT_BIT_WORDS(KEY_MAX + 1U)];
};

static void test_evdev_bits_set(struct test_evdev_bits *bits, unsigned int code) {
    bits->words[code / INKCELL_INPUT_BITS_PER_LONG] |= 1UL << (code % INKCELL_INPUT_BITS_PER_LONG);
}

/*
 * The filter that keeps a pad from being crowded out.
 *
 * Taking the first few nodes that open worked on a Brick by accident - it has three or four and
 * one of them is the pad. On a host with a keyboard, a mouse, two trackpads and an
 * accelerometer, the pad is not necessarily among them, and the client comes up with no buttons
 * at all. Plugging a USB keyboard into a Brick is the same thing one node at a time.
 */
MESH_TEST_CASE(input_device_filter_keeps_the_pad, unit) {
    struct test_evdev_bits keys;
    struct test_evdev_bits axes;
    const size_t words = INKWELL_ARRAY_LEN(keys.words);

    /* A pad: the face buttons are keys and the d-pad is a pair of absolute axes. */
    memset(&keys, 0, sizeof keys);
    test_evdev_bits_set(&keys, BTN_EAST);
    MESH_TEST_FAIL_IF(!inkcell_input_device_wanted(keys.words, words, NULL, 0U),
                      "a node reporting a face button is the pad");

    memset(&axes, 0, sizeof axes);
    test_evdev_bits_set(&axes, ABS_HAT0X);
    MESH_TEST_FAIL_IF(!inkcell_input_device_wanted(NULL, 0U, axes.words, words),
                      "a node reporting the d-pad hat is worth watching");

    /* The Brick's PMIC: one key, and one this client deliberately does not answer, because a
       tap of the power button is the console's sleep gesture. */
    memset(&keys, 0, sizeof keys);
    test_evdev_bits_set(&keys, KEY_POWER);
    memset(&axes, 0, sizeof axes);
    MESH_TEST_FAIL_IF(inkcell_input_device_wanted(keys.words, words, axes.words, words),
                      "a node whose only key is KEY_POWER is not one we read");

    /*
     * The triggers, which are the second control here that is not a button. A node exposing
     * only those - a split input device, or a driver that puts them on a node of their own -
     * used to be opened, asked what it reported, and closed, with the mapping for them
     * unreachable one function away. The filter names no axis of its own now; it asks
     * inkcell_input_reads_axis(), which is the same answer the event path acts on.
     */
    memset(&keys, 0, sizeof keys);
    memset(&axes, 0, sizeof axes);
    test_evdev_bits_set(&axes, ABS_Z);
    test_evdev_bits_set(&axes, ABS_RZ);
    MESH_TEST_FAIL_IF(!inkcell_input_device_wanted(keys.words, words, axes.words, words),
                      "a node reporting only the triggers is worth watching");
    MESH_TEST_FAIL_IF(!inkcell_input_reads_axis(ABS_Z) || !inkcell_input_reads_axis(ABS_RZ) ||
                          !inkcell_input_reads_axis(ABS_HAT0Y),
                      "the triggers and the hat are the axes this client reads");

    /* A mouse or a trackpad: absolute axes, none of them a hat or a trigger. */
    memset(&keys, 0, sizeof keys);
    memset(&axes, 0, sizeof axes);
    test_evdev_bits_set(&axes, ABS_X);
    test_evdev_bits_set(&axes, ABS_Y);
    MESH_TEST_FAIL_IF(inkcell_input_device_wanted(keys.words, words, axes.words, words),
                      "a pointer's axes are not the d-pad");
    MESH_TEST_FAIL_IF(inkcell_input_reads_axis(ABS_X) || inkcell_input_reads_axis(ABS_Y),
                      "a pointer's axes mean nothing here");

    /* A node that cannot answer is watched: being unable to tell is not evidence of a useless
       device, and the two failures cost very different things. */
    MESH_TEST_FAIL_IF(!inkcell_input_device_wanted(NULL, 0U, NULL, 0U),
                      "a node that cannot say what it reports should still be watched");

    record_success(test_name);
}

/*
 * The filter has to follow the quit keys as well as the profile, or an override moves quitting
 * onto a node the filter has just dropped - which is a client that cannot be left.
 */
MESH_TEST_CASE(input_device_filter_follows_the_quit_keys, unit) {
    struct test_evdev_bits keys;
    const size_t words = INKWELL_ARRAY_LEN(keys.words);
    const char *failure = NULL;

    memset(&keys, 0, sizeof keys);
    test_evdev_bits_set(&keys, KEY_POWER);

    unsetenv("MESHCLIENT_QUIT_KEYS");
    inkcell_input_reload_quit_keys();
    if (inkcell_input_device_wanted(keys.words, words, NULL, 0U)) {
        failure = "KEY_POWER is not read by default";
        goto restore;
    }

    /* Somebody has moved quitting onto the power button. The node holding it is now the only
       way out of the client, so the filter must keep it. */
    setenv("MESHCLIENT_QUIT_KEYS", "116", 1);
    inkcell_input_reload_quit_keys();
    if (!inkcell_input_device_wanted(keys.words, words, NULL, 0U)) {
        failure = "a node holding the configured quit key must be watched";
    }

restore:
    unsetenv("MESHCLIENT_QUIT_KEYS");
    inkcell_input_reload_quit_keys();
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

struct test_animation_backend {
    unsigned frames;
    char status[128];
    mesh_ui_update_flags flags;
};

static void test_animation_present(void *state, const void *snapshot_ptr, void *userdata) {
    /* The vtable's snapshot is a void *: inkcell hands a backend whatever the application
       publishes and never looks inside it. */
    const struct mesh_ui_snapshot *const snapshot = (const struct mesh_ui_snapshot *)snapshot_ptr;
    (void)state;
    struct test_animation_backend *capture = userdata;
    ++capture->frames;
    snprintf(capture->status, sizeof capture->status, "%s", snapshot->transport_status);
    capture->flags = snapshot->update_flags;
}

static bool test_animation_moving(void *state, void *userdata) {
    (void)state;
    (void)userdata;
    return true;
}

/*
 * A backend that refuses to open is not a controller that failed to start.
 *
 * The distinction the composition root depends on. mesh_ui_controller_init() drops a backend
 * whose init() refused and returns success, because a run with no UI is still a run - but that
 * means its return value cannot be read as "there is a panel", and a client that had a second
 * backend to try would otherwise commit to the first and end up presenting nothing. With the
 * SDL backend there is a second one to try and a dozen ways for a window to refuse after the
 * library has said it is available, so this is the seam mesh_app_init() asks about before it
 * decides the UI is settled.
 */
static int test_refusing_init(void **state, void *userdata) {
    (void)state;
    (void)userdata;
    return -ENODEV;
}

MESH_TEST_CASE(ui_controller_reports_a_backend_that_would_not_open, unit) {
    struct inkwell_loop loop;
    struct mesh_ui_store store;
    struct mesh_ui_controller controller;
    const struct inkcell_backend refusing = {.name = "test-refusing", .init = test_refusing_init};
    MESH_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    if (mesh_ui_store_init(&store) != 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "store init failed");
        return;
    }

    const char *failure = NULL;
    /* Success, deliberately: the controller is up and the store is being watched. */
    if (mesh_ui_controller_init(&controller, &store, &refusing, NULL, &loop) != 0) {
        failure = "a refused backend must not stop the controller starting";
    } else if (mesh_ui_controller_has_backend(&controller)) {
        failure = "...but the controller must not claim a backend that refused";
    }
    mesh_ui_controller_shutdown(&controller);

    /* ...and one that opens says so, or the answer above means nothing. */
    if (failure == NULL) {
        const struct inkcell_backend opening = {.name = "test-opening"};
        if (mesh_ui_controller_init(&controller, &store, &opening, NULL, &loop) != 0) {
            failure = "controller init failed";
        } else if (!mesh_ui_controller_has_backend(&controller)) {
            failure = "a backend that opened must be reported as present";
        }
        mesh_ui_controller_shutdown(&controller);
    }

    mesh_ui_store_shutdown(&store);
    inkwell_loop_shutdown(&loop);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Animation through the real store: the scheduler is inkstand's, and holds its own cases - a
   request joining a frame already armed among them. What is this client's is the drain and the
   "nothing changed" hook, which have to leave the store's pending flags clear and the snapshot's
   update flags empty on a frame that changed nothing. */
MESH_TEST_CASE(ui_controller_animation_reuses_snapshot_and_consumes_changes, unit) {
    struct inkwell_loop loop;
    struct mesh_ui_store store;
    struct mesh_ui_controller controller;
    struct test_animation_backend capture = {0};
    const struct inkcell_backend backend = {
        .name = "test-animation",
        .present = test_animation_present,
        .animating = test_animation_moving,
    };
    MESH_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    if (mesh_ui_store_init(&store) != 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "store init failed");
        return;
    }
    if (mesh_ui_controller_init(&controller, &store, &backend, &capture, &loop) != 0) {
        mesh_ui_store_shutdown(&store);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "controller init failed");
        return;
    }
    const char *failure = NULL;
    mesh_ui_store_set_transport_status(&store, "initial");
    inkwell_loop_run(&loop, 0);
    for (unsigned pass = 0U; pass < 2U; ++pass) {
        if (inkwell_timer_arm_once(controller.frames.timer_fd, 1U) < 0) {
            failure = "frame timer could not be armed";
            break;
        }
        struct pollfd poll_fd = {.fd = controller.frames.timer_fd, .events = POLLIN};
        if (poll(&poll_fd, 1, 1000) != 1) {
            failure = "frame timer did not become ready";
            break;
        }
        if (pass == 1U) {
            mesh_ui_store_set_transport_status(&store, "changed");
        }
        inkwell_loop_run(&loop, 0);
        if (store.pending_flags != MESH_UI_UPDATE_NONE ||
            strcmp(capture.status, pass == 0U ? "initial" : "changed") != 0 ||
            (pass == 0U && capture.flags != MESH_UI_UPDATE_NONE)) {
            failure = "animation must reuse clean data and consume concurrent store changes";
            break;
        }
    }
    if (capture.frames < 3U) {
        failure = "timer frames were not presented";
    }
    mesh_ui_controller_shutdown(&controller);
    mesh_ui_store_shutdown(&store);
    inkwell_loop_shutdown(&loop);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
