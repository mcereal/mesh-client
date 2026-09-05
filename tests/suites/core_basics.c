#define _POSIX_C_SOURCE 200809L

/* Config defaults, the event loop's lifecycle, and transport registration. */

#include "framework/mesh_test.h"

#include "mesh/core/config.h"
#include "mesh/core/event_loop.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/transport.h"

#include <errno.h>

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
