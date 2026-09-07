#define _POSIX_C_SOURCE 200809L

/* Config defaults, the event loop's lifecycle, and transport registration. */

#include "framework/mesh_test.h"

#include "mesh/core/config.h"
#include "mesh/core/event_loop.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/transport.h"

#include "mesh/utils/time.h"

#include <errno.h>
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

    const uint64_t started_ms = mesh_time_monotonic_ms();
    const int result = mesh_event_loop_run(&loop, 100);
    const uint64_t elapsed_ms = mesh_time_monotonic_ms() - started_ms;

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
