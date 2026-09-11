#define _POSIX_C_SOURCE 200809L

/* evdev key mapping, the Brick's face buttons, and controller dispatch. */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/core/event_loop.h"
#include "mesh/core/message.h"
#include "mesh/ui/actions.h"
#include "mesh/ui/backend.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/stub.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/input.h"
#include "mesh/ui/input_profile.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"
#include "mesh/utils/array.h"

#include <errno.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <sys/timerfd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

    /* Regression: KEY_POWER used to quit. The Brick's PMIC emits it on a tap of the power
       button, which is this hardware's sleep gesture, so the client died instead of the
       console suspending. Measured on-device with `make deploy-input-map`. */
    MESH_TEST_FAIL_IF(mesh_ui_input_is_quit_key(KEY_POWER),
                      "KEY_POWER should not quit: it is the Brick's sleep gesture");

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
    /* A direction's kernel autorepeat is dropped - our own timer drives those, and honouring
       both would take two rows per step. A face button's still counts. */
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_DOWN, 2);
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_ENTER, 2);
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_SELECT, 1);
    mesh_ui_input_handle_event(&input, EV_SYN, 0, 0);
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_F1, 1); /* unmapped: nothing */

    /* BTN_SOUTH is the Brick's B and BTN_EAST its A (Nintendo layout). */
    const enum mesh_ui_key expected[] = {
        MESH_UI_KEY_B,  MESH_UI_KEY_A, MESH_UI_KEY_UP,     MESH_UI_KEY_RIGHT,
        MESH_UI_KEY_L1, MESH_UI_KEY_A, MESH_UI_KEY_SELECT,
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

/* Holding the d-pad has to scroll. The Brick reports the d-pad as ABS_HAT0X/Y, and an absolute
 * axis gets no kernel autorepeat however long it is held - one event out of centre, one back -
 * so walking a 60-node roster used to be 60 presses. The repeat is generated in the input layer
 * instead. No timerfd here: a zeroed struct has none, and the hold is stepped by hand. */
MESH_TEST_CASE(ui_input_key_repeat, unit) {
    const char *failure = NULL;
    unsetenv("MESHCLIENT_QUIT_KEYS");
    unsetenv("MESHCLIENT_KEY_REPEAT_DELAY_MS");
    unsetenv("MESHCLIENT_KEY_REPEAT_MS");
    mesh_ui_input_reload_quit_keys();
    mesh_ui_input_reload_key_repeat();

    /* The first repeat waits out the hold delay; later ones come at the scroll interval, and
       the interval ramps down once the hold is clearly deliberate. */
    const unsigned int hold = mesh_ui_input_repeat_delay_ms(0U);
    const unsigned int step = mesh_ui_input_repeat_delay_ms(1U);
    const unsigned int fast = mesh_ui_input_repeat_delay_ms(64U);
    MESH_TEST_FAIL_IF(hold == 0U || step == 0U || fast == 0U, "repeat should be on by default");
    MESH_TEST_FAIL_IF(hold <= step, "the hold delay must be longer than the scroll interval");
    MESH_TEST_FAIL_IF(fast > step, "a long hold must not scroll slower than a short one");

    struct test_key_capture capture;
    memset(&capture, 0, sizeof capture);
    struct mesh_ui_input input;
    memset(&input, 0, sizeof input);
    mesh_ui_input_set_handler(&input, test_capture_key, &capture);

    /* Down on the hat: one row now, and the hold that will produce the rest. */
    mesh_ui_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 1);
    if (capture.count != 1U || capture.keys[0] != MESH_UI_KEY_DOWN ||
        mesh_ui_input_repeat_key(&input) != MESH_UI_KEY_DOWN) {
        failure = "a held direction should emit once and arm the repeat";
        goto cleanup;
    }

    mesh_ui_input_repeat_tick(&input);
    mesh_ui_input_repeat_tick(&input);
    if (capture.count != 3U || capture.keys[1] != MESH_UI_KEY_DOWN ||
        capture.keys[2] != MESH_UI_KEY_DOWN) {
        failure = "each repeat should emit the held direction again";
        goto cleanup;
    }

    /* Centre is the release, and nothing repeats after it. */
    mesh_ui_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 0);
    mesh_ui_input_repeat_tick(&input);
    if (capture.count != 3U || mesh_ui_input_repeat_key(&input) != MESH_UI_KEY_NONE) {
        failure = "centring the hat should end the repeat";
        goto cleanup;
    }

    /* A keyboard's arrow key repeats through the same path, and the kernel's own autorepeat is
       dropped rather than counted twice - a direction is driven by our timer or by nothing. */
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_DOWN, 1);
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_DOWN, 2);
    if (capture.count != 4U || mesh_ui_input_repeat_key(&input) != MESH_UI_KEY_DOWN) {
        failure = "kernel autorepeat must not double a repeat we are already driving";
        goto cleanup;
    }
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_DOWN, 0);
    if (mesh_ui_input_repeat_key(&input) != MESH_UI_KEY_NONE) {
        failure = "releasing the key should end the repeat";
        goto cleanup;
    }

    /* A device unplugged mid-hold never sends the release, so losing its fd has to end the hold
       - otherwise the timer scrolls the list until some other button is pressed. */
    mesh_ui_input_handle_device_event(&input, 7, EV_KEY, KEY_DOWN, 1);
    mesh_ui_input_device_lost(&input, 9);
    if (mesh_ui_input_repeat_key(&input) != MESH_UI_KEY_DOWN) {
        failure = "another device going away must not end this hold";
        goto cleanup;
    }
    mesh_ui_input_device_lost(&input, 7);
    if (mesh_ui_input_repeat_key(&input) != MESH_UI_KEY_NONE) {
        failure = "losing the device that started a hold should end it";
        goto cleanup;
    }

    /* Confirm and back never repeat: a held A that fired forty times would open forty things.
       Pressing one also ends a direction still being held. */
    mesh_ui_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 1);
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_EAST, 1);
    if (mesh_ui_input_repeat_key(&input) != MESH_UI_KEY_NONE) {
        failure = "A should not repeat, and should stop the direction that was held";
        goto cleanup;
    }

    /* The knobs exist because the device has no console; 0 restores one row per press. Off has
       to mean off on a keyboard too, so the kernel's autorepeat stays dropped. */
    setenv("MESHCLIENT_KEY_REPEAT_DELAY_MS", "0", 1);
    mesh_ui_input_reload_key_repeat();
    mesh_ui_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 1);
    if (mesh_ui_input_repeat_delay_ms(0U) != 0U ||
        mesh_ui_input_repeat_key(&input) != MESH_UI_KEY_NONE) {
        failure = "a zero delay should switch hold-to-scroll off";
        goto cleanup;
    }

    const size_t before_hold = capture.count;
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_DOWN, 1);
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_DOWN, 2);
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_DOWN, 2);
    if (capture.count != before_hold + 1U) {
        failure = "with repeat off, holding a key should still move exactly one row";
        goto cleanup;
    }

cleanup:
    unsetenv("MESHCLIENT_KEY_REPEAT_DELAY_MS");
    unsetenv("MESHCLIENT_KEY_REPEAT_MS");
    mesh_ui_input_reload_key_repeat();
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
       reaches the handler once. A opens the conversation, A again the quick replies, A once
       more sends the row the cursor starts on. */
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_LEFT);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_DOWN);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_A);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_A);
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

/*
 * The registry has to hold its shape, for the reason the theme table does: anyone may add a row
 * and nothing else in the build would notice a row that bound three face buttons.
 */
MESH_TEST_CASE(input_profiles_are_complete, unit) {
    const size_t count = mesh_ui_input_profile_count();
    MESH_TEST_FAIL_IF(count == 0U, "the profile registry is empty");
    MESH_TEST_FAIL_IF(mesh_ui_input_profile_default() == NULL, "there is no default profile");

    for (size_t i = 0; i < count; ++i) {
        const struct mesh_ui_input_profile *profile = mesh_ui_input_profile_at(i);
        char reason[128];
        if (profile == NULL) {
            record_failure(test_name, "the registry has a hole in it");
            return;
        }
        if (!mesh_ui_input_profile_validate(profile, reason, sizeof reason)) {
            record_failure(test_name, reason);
            return;
        }
        /* Two rows with one name is a profile nobody can select, since the lookup takes the
           first match and says nothing about the second. */
        for (size_t j = i + 1U; j < count; ++j) {
            const struct mesh_ui_input_profile *other = mesh_ui_input_profile_at(j);
            if (other != NULL && strcasecmp(profile->name, other->name) == 0) {
                char detail[96];
                snprintf(detail, sizeof detail, "two profiles are called %s", profile->name);
                record_failure(test_name, detail);
                return;
            }
        }
    }

    /* The pak ships for the Brick, so that is what an unconfigured client must be. */
    MESH_TEST_FAIL_IF(strcmp(mesh_ui_input_profile_default()->name, "brick") != 0,
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
    const struct mesh_ui_input_profile *brick = mesh_ui_input_profile_by_name("brick");
    const struct mesh_ui_input_profile *xbox = mesh_ui_input_profile_by_name("xbox");
    MESH_TEST_FAIL_IF(brick == NULL || xbox == NULL, "both shipped profiles should resolve");

    MESH_TEST_FAIL_IF(mesh_ui_input_profile_key(brick, BTN_EAST) != MESH_UI_KEY_A,
                      "the Brick's A is BTN_EAST");
    MESH_TEST_FAIL_IF(mesh_ui_input_profile_key(brick, BTN_SOUTH) != MESH_UI_KEY_B,
                      "the Brick's B is BTN_SOUTH");
    MESH_TEST_FAIL_IF(mesh_ui_input_profile_key(xbox, BTN_SOUTH) != MESH_UI_KEY_A,
                      "an Xbox-convention A is BTN_SOUTH");
    MESH_TEST_FAIL_IF(mesh_ui_input_profile_key(xbox, BTN_EAST) != MESH_UI_KEY_B,
                      "an Xbox-convention B is BTN_EAST");

    /* Name resolution is how a launch.sh sets this, so it takes the spelling a person types. */
    MESH_TEST_FAIL_IF(mesh_ui_input_profile_by_name("XBOX") != xbox,
                      "profile names should be case-insensitive");
    MESH_TEST_FAIL_IF(mesh_ui_input_profile_by_name("") != NULL ||
                          mesh_ui_input_profile_by_name(NULL) != NULL ||
                          mesh_ui_input_profile_by_name("no-such-pad") != NULL,
                      "an unknown name should not resolve");
    record_success(test_name);
}

/*
 * The regression this whole table exists for.
 *
 * The codes lived in src/ui/input.c and the caps in src/ui/actions.c, and a port that corrected
 * one and not the other leaves the action bar naming a key that does something else - which is
 * invisible, because the binding still works. Both public entry points are asked here, under a
 * profile that is not the default, so the two cannot come from different places again.
 */
MESH_TEST_CASE(input_profile_caps_and_codes_move_together, unit) {
    const char *failure = NULL;

    setenv("MESHCLIENT_INPUT_PROFILE", "xbox", 1);
    mesh_ui_input_profile_reload();

    if (mesh_ui_input_map_key(BTN_SOUTH) != MESH_UI_KEY_A ||
        mesh_ui_input_map_key(BTN_EAST) != MESH_UI_KEY_B) {
        failure = "the selected profile should decide what the face buttons report";
        goto restore;
    }
    if (strcmp(mesh_ui_button_cap(MESH_UI_BUTTON_A), "A") != 0 ||
        strcmp(mesh_ui_button_cap(MESH_UI_BUTTON_B), "B") != 0) {
        failure = "the cap should come from the profile the codes came from";
        goto restore;
    }
    /* A convention is not a profile's to restate, and must survive the switch. */
    if (mesh_ui_input_map_key(KEY_ENTER) != MESH_UI_KEY_A ||
        mesh_ui_input_map_key(BTN_TL) != MESH_UI_KEY_L1) {
        failure = "the shared conventions should answer under any profile";
        goto restore;
    }

    /* A typo is a wrong label, not a client that cannot be driven: it falls back rather than
       leaving the face buttons bound to nothing. */
    setenv("MESHCLIENT_INPUT_PROFILE", "no-such-pad", 1);
    mesh_ui_input_profile_reload();
    if (mesh_ui_input_map_key(BTN_EAST) != MESH_UI_KEY_A) {
        failure = "an unknown profile should fall back to the default";
    }

restore:
    unsetenv("MESHCLIENT_INPUT_PROFILE");
    mesh_ui_input_profile_reload();
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    /* And back where every other test in this binary expects to find it. */
    MESH_TEST_FAIL_IF(mesh_ui_input_map_key(BTN_EAST) != MESH_UI_KEY_A,
                      "the unconfigured client should be the Brick");
    record_success(test_name);
}

/* An EVIOCGBIT bitmap with one code set, which is how a node says what it can report. */
struct test_evdev_bits {
    unsigned long words[MESH_UI_INPUT_BIT_WORDS(KEY_MAX + 1U)];
};

static void test_evdev_bits_set(struct test_evdev_bits *bits, unsigned int code) {
    bits->words[code / MESH_UI_INPUT_BITS_PER_LONG] |= 1UL << (code % MESH_UI_INPUT_BITS_PER_LONG);
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
    const size_t words = MESH_ARRAY_LEN(keys.words);

    /* A pad: the face buttons are keys and the d-pad is a pair of absolute axes. */
    memset(&keys, 0, sizeof keys);
    test_evdev_bits_set(&keys, BTN_EAST);
    MESH_TEST_FAIL_IF(!mesh_ui_input_device_wanted(keys.words, words, NULL, 0U),
                      "a node reporting a face button is the pad");

    memset(&axes, 0, sizeof axes);
    test_evdev_bits_set(&axes, ABS_HAT0X);
    MESH_TEST_FAIL_IF(!mesh_ui_input_device_wanted(NULL, 0U, axes.words, words),
                      "a node reporting the d-pad hat is worth watching");

    /* The Brick's PMIC: one key, and one this client deliberately does not answer, because a
       tap of the power button is the console's sleep gesture. */
    memset(&keys, 0, sizeof keys);
    test_evdev_bits_set(&keys, KEY_POWER);
    memset(&axes, 0, sizeof axes);
    MESH_TEST_FAIL_IF(mesh_ui_input_device_wanted(keys.words, words, axes.words, words),
                      "a node whose only key is KEY_POWER is not one we read");

    /* A mouse or a trackpad: absolute axes, none of them a hat. */
    memset(&keys, 0, sizeof keys);
    memset(&axes, 0, sizeof axes);
    test_evdev_bits_set(&axes, ABS_X);
    test_evdev_bits_set(&axes, ABS_Y);
    MESH_TEST_FAIL_IF(mesh_ui_input_device_wanted(keys.words, words, axes.words, words),
                      "a pointer's axes are not the d-pad");

    /* A node that cannot answer is watched: being unable to tell is not evidence of a useless
       device, and the two failures cost very different things. */
    MESH_TEST_FAIL_IF(!mesh_ui_input_device_wanted(NULL, 0U, NULL, 0U),
                      "a node that cannot say what it reports should still be watched");

    record_success(test_name);
}

/*
 * The filter has to follow the quit keys as well as the profile, or an override moves quitting
 * onto a node the filter has just dropped - which is a client that cannot be left.
 */
MESH_TEST_CASE(input_device_filter_follows_the_quit_keys, unit) {
    struct test_evdev_bits keys;
    const size_t words = MESH_ARRAY_LEN(keys.words);
    const char *failure = NULL;

    memset(&keys, 0, sizeof keys);
    test_evdev_bits_set(&keys, KEY_POWER);

    unsetenv("MESHCLIENT_QUIT_KEYS");
    mesh_ui_input_reload_quit_keys();
    if (mesh_ui_input_device_wanted(keys.words, words, NULL, 0U)) {
        failure = "KEY_POWER is not read by default";
        goto restore;
    }

    /* Somebody has moved quitting onto the power button. The node holding it is now the only
       way out of the client, so the filter must keep it. */
    setenv("MESHCLIENT_QUIT_KEYS", "116", 1);
    mesh_ui_input_reload_quit_keys();
    if (!mesh_ui_input_device_wanted(keys.words, words, NULL, 0U)) {
        failure = "a node holding the configured quit key must be watched";
    }

restore:
    unsetenv("MESHCLIENT_QUIT_KEYS");
    mesh_ui_input_reload_quit_keys();
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

struct test_animation_backend {
    unsigned frames;
    char status[128];
    mesh_ui_update_flags flags;
};

static void test_animation_present(void *state, const struct mesh_ui_snapshot *snapshot,
                                   void *userdata) {
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

MESH_TEST_CASE(ui_controller_animation_reuses_snapshot_and_consumes_changes, unit) {
    struct mesh_event_loop loop;
    struct mesh_ui_store store;
    struct mesh_ui_controller controller;
    struct test_animation_backend capture = {0};
    const struct mesh_ui_backend backend = {
        .name = "test-animation",
        .present = test_animation_present,
        .animating = test_animation_moving,
    };
    MESH_TEST_FAIL_IF(mesh_event_loop_init(&loop) != 0, "loop init failed");
    if (mesh_ui_store_init(&store) != 0) {
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "store init failed");
        return;
    }
    if (mesh_ui_controller_init(&controller, &store, &backend, &capture, &loop) != 0) {
        mesh_ui_store_shutdown(&store);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "controller init failed");
        return;
    }
    const char *failure = NULL;
    mesh_ui_store_set_transport_status(&store, "initial");
    mesh_event_loop_run(&loop, 0);
    for (unsigned pass = 0U; pass < 2U; ++pass) {
        const struct itimerspec spec = {.it_value = {.tv_nsec = 1L}};
        if (timerfd_settime(controller.frame_timer_fd, 0, &spec, NULL) < 0) {
            failure = "frame timer could not be armed";
            break;
        }
        struct pollfd poll_fd = {.fd = controller.frame_timer_fd, .events = POLLIN};
        if (poll(&poll_fd, 1, 1000) != 1) {
            failure = "frame timer did not become ready";
            break;
        }
        if (pass == 1U) {
            mesh_ui_store_set_transport_status(&store, "changed");
        }
        mesh_event_loop_run(&loop, 0);
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
    mesh_event_loop_shutdown(&loop);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}
