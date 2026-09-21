#define _POSIX_C_SOURCE 200809L

/* Config defaults, the event loop's lifecycle, and transport registration. */

#include "framework/mesh_test.h"

#include "mesh/core/config.h"
#include "mesh/core/event_loop.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/transport.h"

#include "mesh/utils/time.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

MESH_TEST_CASE(config_defaults, unit) {
    struct mesh_app_config config = mesh_app_config_default();
    MESH_TEST_FAIL_IF(config.run_mode != MESH_APP_RUN_SINGLE_POLL,
                      "run_mode should default to single poll");
    MESH_TEST_FAIL_IF(!config.enable_ble, "BLE should be enabled by default");
    MESH_TEST_FAIL_IF(config.idle_timeout_ms != 1000, "idle timeout should default to 1000 ms");
    record_success(test_name);
}

/*
 * The documented transport knobs still answer to their documented names.
 *
 * Worth a case of its own because the way this breaks is invisible: inkcell_env_bool() takes a
 * *suffix* and puts the application's prefix on it, so a caller passing the whole name asks for
 * MESHCLIENT_MESHCLIENT_DISABLE_BLE. That compiles, reads correctly at the call site, and
 * returns the fallback on every machine - so BLE stays on, the client looks fine, and the only
 * symptom is that a knob in docs/cli.md does nothing. One round of the extraction shipped
 * exactly that in five places; a second review found three more.
 *
 * Asserted through mesh_app_config_apply_env_overrides() rather than by reading the helper,
 * because what is being checked is the whole path from the variable a user exports to the field
 * the transports read.
 */
MESH_TEST_CASE(config_disable_overrides_answer_to_their_documented_names, unit) {
    /* Offsets rather than pointers, because the config is rebuilt between knobs and a pointer
       taken before that would be into the previous one. */
    static const struct {
        const char *name;
        size_t offset;
    } knobs[] = {
        {"MESHCLIENT_DISABLE_BLE", offsetof(struct mesh_app_config, enable_ble)},
        {"MESHCLIENT_DISABLE_SERIAL", offsetof(struct mesh_app_config, enable_serial)},
        {"MESHCLIENT_DISABLE_TCP", offsetof(struct mesh_app_config, enable_tcp)},
    };

    struct mesh_app_config config;
    for (size_t i = 0U; i < sizeof knobs / sizeof knobs[0]; ++i) {
        config = mesh_app_config_default();
        MESH_TEST_FAIL_IF(setenv(knobs[i].name, "true", 1) != 0, "setenv failed");
        mesh_app_config_apply_env_overrides(&config);
        const bool still_on = *(const bool *)((const char *)&config + knobs[i].offset);
        (void)unsetenv(knobs[i].name);
        MESH_TEST_FAIL_IF(still_on, "a documented DISABLE_* variable did not disable anything");
    }

    /* And the doubly-prefixed name must not work, or the check above would pass on a build that
       had the bug and a stray variable set. */
    config = mesh_app_config_default();
    MESH_TEST_FAIL_IF(setenv("MESHCLIENT_MESHCLIENT_DISABLE_BLE", "true", 1) != 0, "setenv failed");
    mesh_app_config_apply_env_overrides(&config);
    const bool ble_on = config.enable_ble;
    (void)unsetenv("MESHCLIENT_MESHCLIENT_DISABLE_BLE");
    MESH_TEST_FAIL_IF(!ble_on, "a doubly-prefixed variable disabled a transport");

    record_success(test_name);
}

MESH_TEST_CASE(transport_registry_registration, unit) {
    struct mesh_transport_registry registry;
    mesh_transport_registry_init(&registry);

    struct mesh_transport *ble = mesh_ble_transport();
    int result = mesh_transport_registry_register(&registry, ble);
    MESH_TEST_FAIL_IF(result != 0, "expected first BLE registration to succeed");

    result = mesh_transport_registry_register(&registry, ble);
    MESH_TEST_FAIL_IF(result != -EEXIST, "duplicate registration should return -EEXIST");

    record_success(test_name);
}

MESH_TEST_CASE(event_loop_init_shutdown, unit) {
    struct mesh_event_loop loop;
    int result = mesh_event_loop_init(&loop);
    MESH_TEST_FAIL_IF(result < 0, "mesh_event_loop_init failed");

    result = mesh_event_loop_run(&loop, 0);
    if (result < 0) {
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "mesh_event_loop_run should succeed with zero timeout");
        return;
    }

    mesh_event_loop_shutdown(&loop);
    record_success(test_name);
}

/*
 * A source that re-arms itself faster than the caller's timeout must not hold the loop.
 *
 * This is the screen progress bar, reduced: an indeterminate meter keeps the UI's 33 ms frame
 * timer armed for as long as it is drawn, and it is drawn for as long as the handshake it
 * reports is unfinished - which only advances in mesh_transport_registry_tick(), which only
 * runs when this call returns. The loop used to return only on an idle epoll, so the bar
 * starved the work that would have stopped it and the client sat in "sync in progress" for as
 * long as it was left running.
 */
static int rearming_timer_callback(int fd, uint32_t events, void *userdata) {
    (void)events;
    uint64_t expirations = 0U;
    if (read(fd, &expirations, sizeof expirations) < 0 && errno != EAGAIN) {
        return 0;
    }
    *(unsigned *)userdata += 1U;
    struct itimerspec spec = {0};
    spec.it_value.tv_nsec = 2L * 1000000L; /* well inside the run's own timeout */
    (void)timerfd_settime(fd, 0, &spec, NULL);
    return 0;
}

MESH_TEST_CASE(event_loop_run_returns_under_a_hot_source, unit) {
    struct mesh_event_loop loop;
    MESH_TEST_FAIL_IF(mesh_event_loop_init(&loop) < 0, "mesh_event_loop_init failed");

    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "timerfd_create failed");
        return;
    }
    unsigned firings = 0U;
    struct itimerspec spec = {0};
    spec.it_value.tv_nsec = 2L * 1000000L;
    if (mesh_event_loop_add_fd(&loop, fd, EPOLLIN, rearming_timer_callback, &firings) < 0 ||
        timerfd_settime(fd, 0, &spec, NULL) < 0) {
        close(fd);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "could not arm the hot source");
        return;
    }

    const uint64_t started_ms = inkcell_time_monotonic_ms();
    const int result = mesh_event_loop_run(&loop, 100);
    const uint64_t elapsed_ms = inkcell_time_monotonic_ms() - started_ms;

    mesh_event_loop_remove_fd(&loop, fd);
    close(fd);
    mesh_event_loop_shutdown(&loop);

    MESH_TEST_FAIL_IF(result < 0, "mesh_event_loop_run reported an error");
    /* It has to have actually been busy, or the bound proves nothing. */
    MESH_TEST_FAIL_IF(firings < 2U, "the hot source did not keep the loop busy");
    MESH_TEST_FAIL_IF(elapsed_ms > 1000U,
                      "mesh_event_loop_run did not return within its own timeout");
    record_success(test_name);
}
