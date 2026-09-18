#define _POSIX_C_SOURCE 200809L

/* App glue: auto-connect policy, link routing, and settings writes built from UI state. */

#include "../../src/app/app_internal.h"
#include "framework/mesh_test.h"
#include "support/proto_fixture.h"
#include "support/serial_fixture.h"
#include "support/session_fixture.h"
#include <poll.h>
#include <sys/timerfd.h>

#include "mesh/app/app.h"
#include "mesh/core/config.h"
#include "mesh/core/message.h"
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/i18n/strings.h"
#include "mesh/proto/stream_framing.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/serial.h"
#include "mesh/transport/serial_usb.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/map.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/preferences.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/ui/theme.h"

#include "meshtastic/admin.pb.h"
#include "meshtastic/channel.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/module_config.pb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * Points $HOME at a fresh directory and puts the environment in the state an app case needs: the
 * stub UI backend, and no MESHCLIENT_AUTOCONNECT inherited from the developer's shell. Eleven
 * cases set up that same trio by hand, which is eleven places for one of them to be forgotten -
 * and a case that runs against the real $HOME writes the developer's own preferences file.
 *
 * `home` is the caller's buffer rather than static storage because mkdtemp() rewrites its
 * template in place and every case wants the directory name afterwards. False when the directory
 * could not be made; the caller reports that alongside whatever it has already set up to release.
 */
#define APP_TEST_HOME_CAP 64
static bool app_test_home(char *home, size_t cap, const char *tag) {
    if (snprintf(home, cap, "/tmp/mesh_app_%sXXXXXX", tag) >= (int)cap) {
        return false;
    }
    if (mkdtemp(home) == NULL) {
        return false;
    }
    setenv("HOME", home, 1);
    setenv("MESHCLIENT_UI_BACKEND", "stub", 1);
    unsetenv("MESHCLIENT_AUTOCONNECT");
    return true;
}

/* The clock mesh_app_publish_ui_state stamps its toasts with. */
static uint64_t test_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/* The foreground policy: a saved preferred node wins even when a stronger one is in range;
   with nothing saved, the strongest advertiser is used. */
MESH_TEST_CASE(app_autoconnect_policy, unit) {
    const char *failure = NULL;

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:06", .name = "NodeSix", .rssi = -30, .paired = true},
        {.address = "AA:BB:CC:DD:EE:07", .name = "NodeSeven", .rssi = -70, .paired = true},
    };

    uint8_t write_capture[64];
    size_t write_len = 0U;
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 2U,
        .write_capture_buffer = write_capture,
        .write_capture_capacity = sizeof(write_capture),
        .write_capture_length = &write_len,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    /* Keep the app's preference files out of the real $HOME. */
    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "autoconnect")) {
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "mkdtemp failed");
        return;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    /* This test is about BLE ranking; a USB port on the build host now outranks every
       advertiser, so keep the serial link out of it. */
    config.enable_serial = false;
    snprintf(config.preferred_ble_device, sizeof config.preferred_ble_device, "%s", "NodeSeven");

    struct mesh_app app;
    memset(&app, 0, sizeof app);
    bool app_ready = false;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);

    mesh_app_autoconnect(&app);
    const char *connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[1].address) != 0) {
        failure = "preferred node (by name) should win over a stronger one";
        goto cleanup;
    }

    /* Drop the link and the preference: the strongest node should be chosen next. */
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "disconnect failed";
        goto cleanup;
    }
    app.config.preferred_ble_device[0] = '\0';
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[0].address) != 0) {
        failure = "strongest node should be chosen without a preference";
        goto cleanup;
    }

    /* Already connected: another turn must be a no-op rather than a reconnect. */
    write_len = 0U;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (write_len != 0U) {
        failure = "autoconnect should not act while connected";
        goto cleanup;
    }

    /* Not in foreground mode it must never connect. */
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "second disconnect failed";
        goto cleanup;
    }
    app.config.run_mode = MESH_APP_RUN_SINGLE_POLL;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "single-poll mode must not auto-connect";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    {
        char path[256];
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs.handshake", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient", home_dir);
        rmdir(path);
        rmdir(home_dir);
    }
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The node you left at home.
 *
 * BlueZ lists every device object it holds, bonds included, so a radio that is switched off in
 * another building is in the discovery list with its saved address, its saved name and Paired
 * set - and with no RSSI, which as a raw 0 outranks every node that actually answered. Both
 * halves of that are tested here: it must not be the target, and it must not win the fallback.
 */
MESH_TEST_CASE(app_autoconnect_ignores_a_node_out_of_range, unit) {
    const char *failure = NULL;

    /* NodeSeven is the bond with nothing behind it: the 0 is what bluetoothd leaves when it
       has not heard a device in this scan. NodeSix is the radio in your pocket. */
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:07", .name = "NodeSeven", .rssi = 0, .paired = true},
        {.address = "AA:BB:CC:DD:EE:06", .name = "NodeSix", .rssi = -70, .paired = true},
    };

    uint8_t write_capture[64];
    size_t write_len = 0U;
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 2U,
        .write_capture_buffer = write_capture,
        .write_capture_capacity = sizeof(write_capture),
        .write_capture_length = &write_len,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "out_of_range")) {
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "mkdtemp failed");
        return;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_serial = false; /* a USB port on the build host outranks every advertiser */
    snprintf(config.preferred_ble_device, sizeof config.preferred_ble_device, "%s", "NodeSeven");

    struct mesh_app app;
    memset(&app, 0, sizeof app);
    bool app_ready = false;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);

    /* Both radios are ours, NodeSeven most recently. */
    (void)mesh_ui_preferences_note_device(&app.ui_preferences, mock_devices[1].address,
                                          (uint8_t)MESH_UI_DEVICE_BLE);
    (void)mesh_ui_preferences_note_device(&app.ui_preferences, mock_devices[0].address,
                                          (uint8_t)MESH_UI_DEVICE_BLE);

    /* The first turn hears NodeSix and waits: NodeSeven may yet advertise. */
    mesh_app_autoconnect(&app);
    if (mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "the grace period should hold the first turn";
        goto cleanup;
    }

    /* Past the grace a radio of ours that is actually in earshot wins, however loud the bond
       BlueZ is still holding claims to be. */
    app.autoconnect_started_ms = test_now_ms() - 60000U;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    const char *connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[1].address) != 0) {
        failure = "a node that answered the scan should beat a bond that did not";
        goto cleanup;
    }

    /* And with nothing preferred and nothing remembered, the strongest *advertiser* wins - the
       node with no reading at all must not be read as 0 dBm and taken as the loudest. */
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "disconnect failed";
        goto cleanup;
    }
    app.config.preferred_ble_device[0] = '\0';
    memset(&app.ui_preferences.known_devices, 0, sizeof app.ui_preferences.known_devices);
    app.ui_preferences.known_device_count = 0U;
    app.ui_preferences.preferred_device[0] = '\0';
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[1].address) != 0) {
        failure = "an absent RSSI must not win the strongest-node fallback";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    {
        char path[256];
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs.handshake", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient", home_dir);
        rmdir(path);
        rmdir(home_dir);
    }
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* With the saved node absent, the choice between two strangers is signal strength - but the
   choice between a stranger and a radio of your own is not, however much louder the stranger
   is. Ranking is what makes "the one I was using" beat "the one I can hear best". */
MESH_TEST_CASE(app_autoconnect_prefers_a_radio_of_ours, unit) {
    const char *failure = NULL;

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:20", .name = "Stranger", .rssi = -25, .paired = true},
        {.address = "AA:BB:CC:DD:EE:21", .name = "MineOlder", .rssi = -80, .paired = true},
        {.address = "AA:BB:CC:DD:EE:22", .name = "MineRecent", .rssi = -85, .paired = true},
    };

    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 3U,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "own_radio")) {
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "mkdtemp failed");
        return;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_serial = false;

    struct mesh_app app;
    memset(&app, 0, sizeof app);
    bool app_ready = false;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);

    (void)mesh_ui_preferences_note_device(&app.ui_preferences, mock_devices[1].address,
                                          (uint8_t)MESH_UI_DEVICE_BLE);
    (void)mesh_ui_preferences_note_device(&app.ui_preferences, mock_devices[2].address,
                                          (uint8_t)MESH_UI_DEVICE_BLE);

    /* Nothing is preferred, so there is nothing to wait for and no grace to serve. */
    mesh_app_autoconnect(&app);
    const char *connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[2].address) != 0) {
        failure = "the most recently used radio should win over a louder stranger";
        goto cleanup;
    }

    /* Forgetting its pairing takes it out of the ranking, and the radio used before it takes
       the head - not the stranger, which is still the loudest thing in the room. */
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "disconnect failed";
        goto cleanup;
    }
    if (!mesh_ui_preferences_forget_device(&app.ui_preferences, mock_devices[2].address,
                                           (uint8_t)MESH_UI_DEVICE_BLE)) {
        failure = "forget should report a device it removed";
        goto cleanup;
    }
    app.config.preferred_ble_device[0] = '\0';
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[1].address) != 0) {
        failure = "a forgotten radio should fall out of the ranking";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    {
        char path[256];
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs.handshake", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient", home_dir);
        rmdir(path);
        rmdir(home_dir);
    }
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The grace period belongs to a connection attempt, not to the process.
 *
 * A settings write reboots the radio, so the node we were on stops advertising for a few
 * seconds and the drop is immediately followed by a retry. If the window that lets a preferred
 * node show up were armed once at launch, it would be long expired by then - and the second
 * radio on the desk, being in range and known, would take the slot the rebooting one was about
 * to reclaim. Switching radios by hand has to restart it for the same reason.
 *
 * The link here is to a radio that is not the preferred node, so how long that window runs for
 * is still the short grace: which of the two a drop earns is
 * app_autoconnect_holds_the_slot_for_a_rebooting_radio's half of this.
 */
MESH_TEST_CASE(app_autoconnect_grace_survives_a_reconnect, unit) {
    const char *failure = NULL;

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:07", .name = "NodeSeven", .rssi = 0, .paired = true},
        {.address = "AA:BB:CC:DD:EE:06", .name = "NodeSix", .rssi = -70, .paired = true},
    };

    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 2U,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "grace")) {
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "mkdtemp failed");
        return;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_serial = false;
    snprintf(config.preferred_ble_device, sizeof config.preferred_ble_device, "%s", "NodeSeven");

    struct mesh_app app;
    memset(&app, 0, sizeof app);
    bool app_ready = false;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);
    (void)mesh_ui_preferences_note_device(&app.ui_preferences, mock_devices[1].address,
                                          (uint8_t)MESH_UI_DEVICE_BLE);

    /* At launch the short grace applies: ten seconds is past it, and the radio of ours that is
       in earshot wins. */
    app.autoconnect_started_ms = test_now_ms() - 10000U;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (mesh_ble_transport_connected_address(ble) == NULL) {
        failure = "at launch the known radio should win after the short grace";
        goto cleanup;
    }

    /* A turn with the link up re-arms the window for whatever comes after it. */
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (app.autoconnect_started_ms != 0U) {
        failure = "an established link should re-arm the grace period";
        goto cleanup;
    }

    /* So the drop that follows a radio reboot waits for the preferred node again rather than
       taking the other radio the moment it is heard. */
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "disconnect failed";
        goto cleanup;
    }
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "the grace period should hold the turn after a drop";
        goto cleanup;
    }

    /* The link that dropped was not the preferred node's, so the short grace is still what a
       radio of ours in earshot waits out: ten seconds past it, it takes the slot. */
    app.autoconnect_started_ms = test_now_ms() - 10000U;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (mesh_ble_transport_connected_address(ble) == NULL) {
        failure = "a drop of somebody else's link should not lengthen the wait";
        goto cleanup;
    }
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "disconnect failed";
        goto cleanup;
    }

    /* Choosing a radio by hand restarts it too: the window is this node's, not the last one's. */
    app.autoconnect_started_ms = test_now_ms() - 60000U;
    mesh_app_note_connected_device(&app, mock_devices[0].address, (uint8_t)MESH_UI_DEVICE_BLE);
    if (app.autoconnect_started_ms != 0U) {
        failure = "a switch to another radio should restart the grace period";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    {
        char path[256];
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs.handshake", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient", home_dir);
        rmdir(path);
        rmdir(home_dir);
    }
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The slot is still the rebooting radio's when it comes back.
 *
 * A settings write reboots the radio, which is the commonest way a link ends: the node stops
 * advertising for anything up to half a minute. On device the second radio on the desk was in
 * earshot throughout, and with the short grace it took the slot - so the screens after the save,
 * including the next write, were the wrong radio's. Only the preferred node's own link earns
 * that wait: a cable, a network host or another radio of ours leaves the short one in place,
 * which app_autoconnect_grace_survives_a_reconnect and tcp_link_leaves_the_bluetooth_grace_short
 * hold from the other side.
 */
MESH_TEST_CASE(app_autoconnect_holds_the_slot_for_a_rebooting_radio, unit) {
    const char *failure = NULL;

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:07", .name = "NodeSeven", .rssi = -40, .paired = true},
        {.address = "AA:BB:CC:DD:EE:06", .name = "NodeSix", .rssi = -70, .paired = true},
    };

    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 2U,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "reboot")) {
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "mkdtemp failed");
        return;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_serial = false;
    /* The address rather than the name, because that is what the preference holds the moment a
       link is up: mesh_app_note_connected_device() rewrites it to the node actually reached. */
    snprintf(config.preferred_ble_device, sizeof config.preferred_ble_device, "%s",
             mock_devices[0].address);

    struct mesh_app app;
    memset(&app, 0, sizeof app);
    bool app_ready = false;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);
    /* The other radio is one of ours, so it is what the short grace would hand the slot to. */
    (void)mesh_ui_preferences_note_device(&app.ui_preferences, mock_devices[1].address,
                                          (uint8_t)MESH_UI_DEVICE_BLE);

    /* The preferred node is in range, so no wait is involved in taking it. */
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    const char *connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[0].address) != 0) {
        failure = "expected a link to the preferred node";
        goto cleanup;
    }

    /* A turn with that link up is what arms the long wait for whatever follows it. */
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (!app.autoconnect_after_link) {
        failure = "the preferred node's link should arm the long wait";
        goto cleanup;
    }

    /* Then the save lands: the radio reboots, the link goes and it stops advertising. */
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "disconnect failed";
        goto cleanup;
    }
    mock_devices[0].rssi = 0; /* the 0 a node that is no longer heard leaves behind */
    mesh_ble_transport_refresh_devices(ble);

    app.autoconnect_started_ms = test_now_ms() - 10000U;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "the other radio must not take the slot five seconds into a reboot";
        goto cleanup;
    }

    /* Half a minute is the whole of that wait: a radio that really has gone yields the slot. */
    app.autoconnect_started_ms = test_now_ms() - 60000U;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[1].address) != 0) {
        failure = "past the long grace the radio in earshot should be taken";
        goto cleanup;
    }
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "disconnect failed";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    {
        char path[256];
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs.handshake", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient", home_dir);
        rmdir(path);
        rmdir(home_dir);
    }
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(app_settings_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    radio.has_owner = true;
    snprintf(radio.owner.long_name, sizeof radio.owner.long_name, "%s", "Old Name");
    snprintf(radio.owner.short_name, sizeof radio.owner.short_name, "%s", "OLDN");
    radio.owner.public_key.size = 32U;
    radio.owner.public_key.bytes[0] = 0x42U;
    radio.has_telemetry = true;
    radio.telemetry.device_update_interval = 900U;
    radio.telemetry.environment_measurement_enabled = true;
    radio.telemetry.power_update_interval = 777U;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_USER;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_USER_LONG_NAME;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "Brick");
    action.edits[1].field = MESH_UI_FIELD_USER_UNMESSAGEABLE;
    action.edits[1].number = 1U;
    action.edits[2].field = MESH_UI_FIELD_DISPLAY_FLIP; /* wrong section: ignored */
    action.edits[2].number = 1U;

    struct mesh_admin_request write;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.kind != MESH_ADMIN_SET_OWNER ||
                          strcmp(write.payload.owner.long_name, "Brick") != 0 ||
                          strcmp(write.payload.owner.short_name, "OLDN") != 0 ||
                          !write.payload.owner.has_is_unmessagable ||
                          !write.payload.owner.is_unmessagable ||
                          write.payload.owner.public_key.size != 32U ||
                          write.payload.owner.public_key.bytes[0] != 0x42U,
                      "set_owner should be the radio's user plus the edits");

    action.section = MESH_UI_SETTINGS_TELEMETRY;
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_TELEMETRY_INTERVAL;
    action.edits[0].number = 3600U;
    action.edits[1].field = MESH_UI_FIELD_TELEMETRY_DEVICE;
    action.edits[1].number = 1U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.kind != MESH_ADMIN_SET_MODULE_CONFIG ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_TELEMETRY_CONFIG ||
            write.payload.module_config.which_payload_variant !=
                meshtastic_ModuleConfig_telemetry_tag ||
            write.payload.module_config.payload_variant.telemetry.device_update_interval != 3600U ||
            !write.payload.module_config.payload_variant.telemetry.device_telemetry_enabled ||
            !write.payload.module_config.payload_variant.telemetry
                 .environment_measurement_enabled ||
            write.payload.module_config.payload_variant.telemetry.power_update_interval != 777U,
        "set_module_config should keep the fields we do not show");

    radio.has_device = true;
    radio.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;
    radio.device.node_info_broadcast_secs = 10800U;
    action.section = MESH_UI_SETTINGS_DEVICE;
    action.edit_count = 1U;
    action.edits[0].field = MESH_UI_FIELD_DEVICE_TZDEF;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "AST4");
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.kind != MESH_ADMIN_SET_CONFIG ||
            write.type != meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG ||
            write.payload.config.which_payload_variant != meshtastic_Config_device_tag ||
            strcmp(write.payload.config.payload_variant.device.tzdef, "AST4") != 0 ||
            write.payload.config.payload_variant.device.role !=
                meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE ||
            write.payload.config.payload_variant.device.node_info_broadcast_secs != 10800U,
        "set_device_config should carry the timezone and the rest");

    /* Role is an ordinary enum edit, and the LED row is the one field the UI shows inverted:
       "LED heartbeat on" has to become led_heartbeat_disabled = false. */
    radio.device.led_heartbeat_disabled = true;
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_DEVICE_ROLE;
    action.edits[0].number = (uint32_t)meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
    action.edits[1].field = MESH_UI_FIELD_DEVICE_LED_HEARTBEAT;
    action.edits[1].number = 1U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.payload.config.payload_variant.device.role !=
                              meshtastic_Config_DeviceConfig_Role_ROUTER_LATE ||
                          write.payload.config.payload_variant.device.led_heartbeat_disabled,
                      "the device role or the inverted LED row was not applied");

    radio.has_position = true;
    radio.position.position_broadcast_secs = 900U;
    radio.position.gps_update_interval = 120U;
    radio.position.position_flags = 811U; /* not shown; must survive the write */
    action.section = MESH_UI_SETTINGS_POSITION;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_POSITION_GPS_MODE;
    action.edits[0].number = (uint32_t)meshtastic_Config_PositionConfig_GpsMode_DISABLED;
    action.edits[1].field = MESH_UI_FIELD_POSITION_SMART_DISTANCE;
    action.edits[1].number = 250U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.kind != MESH_ADMIN_SET_CONFIG ||
            write.type != meshtastic_AdminMessage_ConfigType_POSITION_CONFIG ||
            write.payload.config.which_payload_variant != meshtastic_Config_position_tag ||
            write.payload.config.payload_variant.position.gps_mode !=
                meshtastic_Config_PositionConfig_GpsMode_DISABLED ||
            write.payload.config.payload_variant.position.broadcast_smart_minimum_distance !=
                250U ||
            write.payload.config.payload_variant.position.position_broadcast_secs != 900U ||
            write.payload.config.payload_variant.position.position_flags != 811U,
        "set_position_config should carry the edits and keep the rest");

    radio.has_power = true;
    radio.power.ls_secs = 300U;
    radio.power.adc_multiplier_override = 2.5f; /* not shown; must survive the write */
    action.section = MESH_UI_SETTINGS_POWER;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_POWER_SAVING;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_POWER_WAIT_BT;
    action.edits[1].number = 30U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.type != meshtastic_AdminMessage_ConfigType_POWER_CONFIG ||
                          write.payload.config.which_payload_variant !=
                              meshtastic_Config_power_tag ||
                          !write.payload.config.payload_variant.power.is_power_saving ||
                          write.payload.config.payload_variant.power.wait_bluetooth_secs != 30U ||
                          write.payload.config.payload_variant.power.ls_secs != 300U ||
                          write.payload.config.payload_variant.power.adc_multiplier_override < 2.4f,
                      "set_power_config should carry the edits and keep the rest");

    /* Power can leave too little Bluetooth on to reconnect, so it asks before it writes. */
    MESH_TEST_FAIL_IF(!mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_POWER),
                      "the Power section should be behind the confirm overlay");

    radio.has_mqtt = true;
    snprintf(radio.mqtt.address, sizeof radio.mqtt.address, "%s", "mqtt.example.org");
    radio.mqtt.proxy_to_client_enabled = true; /* read-only row; must survive the write */
    action.section = MESH_UI_SETTINGS_MQTT;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_MQTT_USERNAME;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "brick");
    action.edits[1].field = MESH_UI_FIELD_MQTT_MAP_REPORTING;
    action.edits[1].number = 1U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.kind != MESH_ADMIN_SET_MODULE_CONFIG ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_MQTT_CONFIG ||
            write.payload.module_config.which_payload_variant != meshtastic_ModuleConfig_mqtt_tag ||
            strcmp(write.payload.module_config.payload_variant.mqtt.username, "brick") != 0 ||
            !write.payload.module_config.payload_variant.mqtt.map_reporting_enabled ||
            strcmp(write.payload.module_config.payload_variant.mqtt.address, "mqtt.example.org") !=
                0 ||
            !write.payload.module_config.payload_variant.mqtt.proxy_to_client_enabled,
        "set_mqtt_config should carry the edits and keep the rest");

    action.section = MESH_UI_SETTINGS_DISPLAY;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOENT,
                      "a section the radio has not sent cannot be written");
    action.section = MESH_UI_SETTINGS_RADIO;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOTSUP,
                      "the Radio section is read-only");
    record_success(test_name);
}

/*
 * The module writes phase 9 completed: the fields that were being dropped, the submessage that
 * needs its presence flag set, and the Modules list refusing to be a write at all.
 */
MESH_TEST_CASE(app_module_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    struct mesh_admin_request write;

    /* Store & Forward: the three server fields that were dropped before phase 9, with the
       heartbeat the UI does show left untouched by the write. */
    radio.has_store_forward = true;
    radio.store_forward.heartbeat = true;
    action.section = MESH_UI_SETTINGS_STORE_FORWARD;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_SF_RECORDS;
    action.edits[0].number = 250U;
    action.edits[1].field = MESH_UI_FIELD_SF_HISTORY_MAX;
    action.edits[1].number = 50U;
    action.edits[2].field = MESH_UI_FIELD_SF_HISTORY_WINDOW;
    action.edits[2].number = 3600U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.kind != MESH_ADMIN_SET_MODULE_CONFIG ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_STOREFORWARD_CONFIG ||
            write.payload.module_config.payload_variant.store_forward.records != 250U ||
            write.payload.module_config.payload_variant.store_forward.history_return_max != 50U ||
            write.payload.module_config.payload_variant.store_forward.history_return_window !=
                3600U ||
            !write.payload.module_config.payload_variant.store_forward.heartbeat,
        "set_store_forward should carry the server fields and keep the rest");

    /* Telemetry: the health trio and the intervals that had no rows before. */
    radio.has_telemetry = true;
    radio.telemetry.device_update_interval = 900U;
    action.section = MESH_UI_SETTINGS_TELEMETRY;
    action.edit_count = 4U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_TELEMETRY_HEALTH;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_TELEMETRY_HEALTH_INTERVAL;
    action.edits[1].number = 1800U;
    action.edits[2].field = MESH_UI_FIELD_TELEMETRY_POWER_SCREEN;
    action.edits[2].number = 1U;
    action.edits[3].field = MESH_UI_FIELD_TELEMETRY_AIR_INTERVAL;
    action.edits[3].number = 3600U;
    action.edits[4].field = MESH_UI_FIELD_TELEMETRY_AIR_SCREEN;
    action.edits[4].number = 1U;
    action.edit_count = 5U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_TELEMETRY_CONFIG ||
            !write.payload.module_config.payload_variant.telemetry.health_measurement_enabled ||
            write.payload.module_config.payload_variant.telemetry.health_update_interval != 1800U ||
            !write.payload.module_config.payload_variant.telemetry.power_screen_enabled ||
            write.payload.module_config.payload_variant.telemetry.air_quality_interval != 3600U ||
            !write.payload.module_config.payload_variant.telemetry.air_quality_screen_enabled ||
            write.payload.module_config.payload_variant.telemetry.device_update_interval != 900U,
        "set_telemetry should carry the health rows and keep the rest");

    /* MapReportSettings is a submessage: without has_map_report_settings nanopb drops it from
       the wire entirely and the firmware keeps whatever it had. */
    radio.has_mqtt = true;
    action.section = MESH_UI_SETTINGS_MQTT;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_MQTT_MAP_INTERVAL;
    action.edits[0].number = 7200U;
    action.edits[1].field = MESH_UI_FIELD_MQTT_MAP_LOCATION;
    action.edits[1].number = 1U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            !write.payload.module_config.payload_variant.mqtt.has_map_report_settings ||
            write.payload.module_config.payload_variant.mqtt.map_report_settings
                    .publish_interval_secs != 7200U ||
            !write.payload.module_config.payload_variant.mqtt.map_report_settings
                 .should_report_location,
        "a map-report edit should mark the submessage present");

    /* The Modules list is a folder, not a section: there is nothing there for Y to write. */
    action.section = MESH_UI_SETTINGS_MODULES;
    action.edit_count = 0U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOTSUP,
                      "the Modules list cannot be written");
    record_success(test_name);
}

/*
 * Phase 10's six modules through the write path. Each is a section that has to pick the right
 * ModuleConfigType and the right union tag; getting either wrong writes the correct bytes into
 * the wrong module, which the radio accepts without complaint.
 */
MESH_TEST_CASE(app_small_module_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    struct mesh_admin_request write;

    radio.has_neighbor_info = true;
    radio.neighbor_info.transmit_over_lora = true; /* not edited; must survive */
    action.section = MESH_UI_SETTINGS_NEIGHBOR_INFO;
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_NEIGHBOR_ENABLED;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_NEIGHBOR_INTERVAL;
    action.edits[1].number = 21600U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.kind != MESH_ADMIN_SET_MODULE_CONFIG ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_NEIGHBORINFO_CONFIG ||
            write.payload.module_config.which_payload_variant !=
                meshtastic_ModuleConfig_neighbor_info_tag ||
            !write.payload.module_config.payload_variant.neighbor_info.enabled ||
            write.payload.module_config.payload_variant.neighbor_info.update_interval != 21600U ||
            !write.payload.module_config.payload_variant.neighbor_info.transmit_over_lora,
        "set_neighbor_info should carry the edits and keep the rest");

    radio.has_range_test = true;
    action.section = MESH_UI_SETTINGS_RANGE_TEST;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_RANGE_TEST_ENABLED;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_RANGE_TEST_SENDER;
    action.edits[1].number = 60U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.type != meshtastic_AdminMessage_ModuleConfigType_RANGETEST_CONFIG ||
                          write.payload.module_config.which_payload_variant !=
                              meshtastic_ModuleConfig_range_test_tag ||
                          !write.payload.module_config.payload_variant.range_test.enabled ||
                          write.payload.module_config.payload_variant.range_test.sender != 60U,
                      "set_range_test should carry the sender interval");

    /* The signed pair: the UI carries an RSSI as a cast uint32_t and this is where it has to
       come back out as the negative int32 the wire wants. */
    radio.has_paxcounter = true;
    action.section = MESH_UI_SETTINGS_PAXCOUNTER;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_PAX_WIFI_THRESHOLD;
    action.edits[0].number = (uint32_t)(int32_t)-90;
    action.edits[1].field = MESH_UI_FIELD_PAX_BLE_THRESHOLD;
    action.edits[1].number = (uint32_t)(int32_t)-65;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_PAXCOUNTER_CONFIG ||
            write.payload.module_config.payload_variant.paxcounter.wifi_threshold != -90 ||
            write.payload.module_config.payload_variant.paxcounter.ble_threshold != -65,
        "an RSSI edit should reach the wire as a negative int32");

    radio.has_tak = true;
    action.section = MESH_UI_SETTINGS_TAK;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_TAK_TEAM;
    action.edits[0].number = (uint32_t)meshtastic_Team_Green;
    action.edits[1].field = MESH_UI_FIELD_TAK_ROLE;
    action.edits[1].number = (uint32_t)meshtastic_MemberRole_Medic;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_TAK_CONFIG ||
            write.payload.module_config.which_payload_variant != meshtastic_ModuleConfig_tak_tag ||
            write.payload.module_config.payload_variant.tak.team != meshtastic_Team_Green ||
            write.payload.module_config.payload_variant.tak.role != meshtastic_MemberRole_Medic,
        "set_tak should carry the team and role");

    radio.has_ambient_lighting = true;
    action.section = MESH_UI_SETTINGS_AMBIENT;
    action.edit_count = 3U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_AMBIENT_LED;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_AMBIENT_RED;
    action.edits[1].number = 255U;
    action.edits[2].field = MESH_UI_FIELD_AMBIENT_CURRENT;
    action.edits[2].number = 15U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_AMBIENTLIGHTING_CONFIG ||
            !write.payload.module_config.payload_variant.ambient_lighting.led_state ||
            write.payload.module_config.payload_variant.ambient_lighting.red != 255U ||
            write.payload.module_config.payload_variant.ambient_lighting.current != 15U,
        "set_ambient_lighting should carry the level and the current");

    radio.has_status_message = true;
    action.section = MESH_UI_SETTINGS_STATUS_MESSAGE;
    action.edit_count = 1U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_STATUS_TEXT;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "on the ridge until dark");
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_STATUSMESSAGE_CONFIG ||
            strcmp(write.payload.module_config.payload_variant.statusmessage.node_status,
                   "on the ridge until dark") != 0,
        "set_status_message should carry the text");

    /* And a section the radio has not sent still cannot be written. */
    mesh_radio_settings_reset(&radio);
    action.section = MESH_UI_SETTINGS_TAK;
    action.edit_count = 0U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOENT,
                      "a module the radio has not sent cannot be written");
    record_success(test_name);
}

/* Phase 11's three through the write path, each pinning its own union tag. */
MESH_TEST_CASE(app_large_module_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    struct mesh_admin_request write;

    radio.has_detection_sensor = true;
    radio.detection_sensor.use_pullup = true; /* not edited; must survive */
    action.section = MESH_UI_SETTINGS_DETECTION;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_DETECT_ENABLED;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_DETECT_PIN;
    action.edits[1].number = 17U;
    action.edits[2].field = MESH_UI_FIELD_DETECT_NAME;
    snprintf(action.edits[2].text, sizeof action.edits[2].text, "%s", "Motion");
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_DETECTIONSENSOR_CONFIG ||
            write.payload.module_config.which_payload_variant !=
                meshtastic_ModuleConfig_detection_sensor_tag ||
            !write.payload.module_config.payload_variant.detection_sensor.enabled ||
            write.payload.module_config.payload_variant.detection_sensor.monitor_pin != 17U ||
            strcmp(write.payload.module_config.payload_variant.detection_sensor.name, "Motion") !=
                0 ||
            !write.payload.module_config.payload_variant.detection_sensor.use_pullup,
        "set_detection_sensor should carry the edits and keep the rest");

    /*
     * External notification's three output groups must reach three different fields. A
     * copy-paste in the apply switch would land two of these on one pin and the wire would
     * carry a config the user never asked for, so all three go out at once with distinct
     * values and all three are checked.
     */
    radio.has_external_notification = true;
    action.section = MESH_UI_SETTINGS_EXT_NOTIFICATION;
    action.edit_count = 6U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_EXTNOTIF_PIN;
    action.edits[0].number = 13U;
    action.edits[1].field = MESH_UI_FIELD_EXTNOTIF_PIN_VIBRA;
    action.edits[1].number = 19U;
    action.edits[2].field = MESH_UI_FIELD_EXTNOTIF_PIN_BUZZER;
    action.edits[2].number = 25U;
    action.edits[3].field = MESH_UI_FIELD_EXTNOTIF_ALERT_MSG;
    action.edits[3].number = 1U;
    action.edits[4].field = MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_VIBRA;
    action.edits[4].number = 1U;
    action.edits[5].field = MESH_UI_FIELD_EXTNOTIF_NAG;
    action.edits[5].number = 60U;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_EXTNOTIF_CONFIG ||
            write.payload.module_config.which_payload_variant !=
                meshtastic_ModuleConfig_external_notification_tag ||
            write.payload.module_config.payload_variant.external_notification.output != 13U ||
            write.payload.module_config.payload_variant.external_notification.output_vibra != 19U ||
            write.payload.module_config.payload_variant.external_notification.output_buzzer !=
                25U ||
            !write.payload.module_config.payload_variant.external_notification.alert_message ||
            write.payload.module_config.payload_variant.external_notification.alert_message_vibra ||
            !write.payload.module_config.payload_variant.external_notification.alert_bell_vibra ||
            write.payload.module_config.payload_variant.external_notification.nag_timeout != 60U,
        "each external-notify output group should reach its own field");

    /* Traffic management has no enabled flag; a zero is a real value meaning off. */
    radio.has_traffic_management = true;
    radio.traffic_management.rate_limit_window_secs = 300U;
    action.section = MESH_UI_SETTINGS_TRAFFIC;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_TRAFFIC_NODEINFO_HOPS;
    action.edits[0].number = 3U;
    action.edits[1].field = MESH_UI_FIELD_TRAFFIC_RATE_WINDOW;
    action.edits[1].number = 0U; /* turning a limit off is a write, not a no-op */
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
            write.type != meshtastic_AdminMessage_ModuleConfigType_TRAFFICMANAGEMENT_CONFIG ||
            write.payload.module_config.which_payload_variant !=
                meshtastic_ModuleConfig_traffic_management_tag ||
            write.payload.module_config.payload_variant.traffic_management
                    .nodeinfo_direct_response_max_hops != 3U ||
            write.payload.module_config.payload_variant.traffic_management.rate_limit_window_secs !=
                0U,
        "set_traffic_management should carry a zero as a value");
    record_success(test_name);
}

/* Key choices become bytes, roles map back, and a bad PIN never reaches the radio. */
MESH_TEST_CASE(app_channel_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    meshtastic_Channel channel = meshtastic_Channel_init_default;
    channel.index = 1;
    channel.role = meshtastic_Channel_Role_SECONDARY;
    channel.has_settings = true;
    channel.settings.psk.size = 1U;
    channel.settings.psk.bytes[0] = 1U;
    channel.settings.id = 77U;
    mesh_radio_settings_apply_channel(&radio, &channel);
    radio.has_bluetooth = true;
    radio.bluetooth.enabled = true;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_CHANNELS;
    action.channel = 1U;
    action.edit_count = 5U;
    action.edits[0].field = MESH_UI_FIELD_CHANNEL_KEY;
    action.edits[0].number = MESH_UI_PSK_RANDOM_256;
    action.edits[1].field = MESH_UI_FIELD_CHANNEL_NAME;
    snprintf(action.edits[1].text, sizeof action.edits[1].text, "%s", "Hikers");
    action.edits[2].field = MESH_UI_FIELD_CHANNEL_ROLE;
    action.edits[2].number = 0U; /* disabled */
    action.edits[3].field = MESH_UI_FIELD_CHANNEL_POSITION;
    action.edits[3].number = 16U;
    /* The other half of the same submessage, which has to be marked present for either field to
       reach the radio - and does not disturb the first when both are edited at once. */
    action.edits[4].field = MESH_UI_FIELD_CHANNEL_MUTED;
    action.edits[4].number = 1U;

    struct mesh_admin_request write;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.kind != MESH_ADMIN_SET_CHANNEL || write.type != 1U ||
                          write.payload.channel.index != 1 ||
                          write.payload.channel.role != meshtastic_Channel_Role_DISABLED ||
                          strcmp(write.payload.channel.settings.name, "Hikers") != 0 ||
                          write.payload.channel.settings.psk.size != 32U ||
                          write.payload.channel.settings.id != 77U ||
                          !write.payload.channel.settings.has_module_settings ||
                          write.payload.channel.settings.module_settings.position_precision !=
                              16U ||
                          !write.payload.channel.settings.module_settings.is_muted,
                      "the channel write should carry the edits over the radio's copy");
    bool all_zero = true;
    for (unsigned i = 0; i < 32U; ++i) {
        if (write.payload.channel.settings.psk.bytes[i] != 0U) {
            all_zero = false;
        }
    }
    MESH_TEST_FAIL_IF(all_zero, "a random key should not be all zeroes");
    action.edit_count = 1U;
    action.edits[0].number = MESH_UI_PSK_TYPED;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s",
             "d4f1bb3a20290759f0bcffabcf4e6901");
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.payload.channel.settings.psk.size != 16U ||
                          write.payload.channel.settings.psk.bytes[0] != 0xD4U ||
                          write.payload.channel.settings.psk.bytes[15] != 0x01U,
                      "a typed key should be parsed as hex");
    action.edits[0].number = MESH_UI_PSK_NONE;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.payload.channel.settings.psk.size != 0U,
                      "no encryption is an empty key");
    action.channel = 3U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOENT,
                      "a slot the radio never sent cannot be written");

    action.section = MESH_UI_SETTINGS_BLUETOOTH;
    action.channel = MESH_UI_SETTINGS_NO_CHANNEL;
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_BT_MODE;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_BT_PIN;
    snprintf(action.edits[1].text, sizeof action.edits[1].text, "%s", "123456");
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.kind != MESH_ADMIN_SET_CONFIG ||
                          write.type != meshtastic_AdminMessage_ConfigType_BLUETOOTH_CONFIG ||
                          write.payload.config.which_payload_variant !=
                              meshtastic_Config_bluetooth_tag ||
                          write.payload.config.payload_variant.bluetooth.mode !=
                              meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN ||
                          write.payload.config.payload_variant.bluetooth.fixed_pin != 123456U ||
                          !write.payload.config.payload_variant.bluetooth.enabled,
                      "the Bluetooth write is wrong");
    snprintf(action.edits[1].text, sizeof action.edits[1].text, "%s", "12ab56");
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -EINVAL,
                      "a PIN that is not six digits must be refused");
    record_success(test_name);
}

/*
 * "Clear this slot": the write that empties a channel rather than editing it.
 *
 * The claim worth holding is that it starts from nothing rather than from the radio's copy.
 * Disabling a channel through its Role row leaves the name and the key in the slot - that is
 * what this verb exists to be the second press for - so a clear built as "the radio's channel
 * with the role changed" would be the bug wearing the fix's name. Every field of the
 * ChannelSettings goes, `id` and the two MQTT flags included: this client has no row for `id`
 * at all, so carrying it is exactly how a field nobody can see survives a press meant to erase
 * everything.
 *
 * The edits are set and deliberately not honoured for the same reason: a name typed a moment
 * before the press is on its way to being erased, and sending it first would put a name the
 * reader is deleting onto the air.
 */
MESH_TEST_CASE(app_channel_clear_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    meshtastic_Channel channel = meshtastic_Channel_init_default;
    channel.index = 1;
    channel.role = meshtastic_Channel_Role_SECONDARY;
    channel.has_settings = true;
    snprintf(channel.settings.name, sizeof channel.settings.name, "%s", "Hikers");
    channel.settings.psk.size = 16U;
    for (unsigned i = 0; i < 16U; ++i) {
        channel.settings.psk.bytes[i] = (uint8_t)(0xA0U + i);
    }
    channel.settings.id = 77U;
    channel.settings.uplink_enabled = true;
    channel.settings.downlink_enabled = true;
    channel.settings.has_module_settings = true;
    channel.settings.module_settings.position_precision = 16U;
    channel.settings.module_settings.is_muted = true;
    mesh_radio_settings_apply_channel(&radio, &channel);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_CHANNELS;
    action.channel = 1U;
    action.number = (uint32_t)MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL;
    action.edit_count = 1U;
    action.edits[0].field = MESH_UI_FIELD_CHANNEL_NAME;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "Renamed");

    struct mesh_admin_request write;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.kind != MESH_ADMIN_SET_CHANNEL || write.type != 1U ||
                          write.payload.channel.index != 1 ||
                          write.payload.channel.role != meshtastic_Channel_Role_DISABLED ||
                          !write.payload.channel.has_settings,
                      "clearing a slot should be a SET_CHANNEL that disables it");
    MESH_TEST_FAIL_IF(write.payload.channel.settings.name[0] != '\0' ||
                          write.payload.channel.settings.psk.size != 0U ||
                          write.payload.channel.settings.id != 0U ||
                          write.payload.channel.settings.uplink_enabled ||
                          write.payload.channel.settings.downlink_enabled ||
                          write.payload.channel.settings.has_module_settings ||
                          write.payload.channel.settings.module_settings.position_precision != 0U ||
                          write.payload.channel.settings.module_settings.is_muted,
                      "clearing a slot should leave nothing of the channel behind");
    /* Not merely a size of 0 with the old key still in the buffer: the bytes go too, so nothing
       downstream can read past the length and find a key that was supposed to be gone. */
    bool any_key_byte = false;
    for (unsigned i = 0; i < sizeof write.payload.channel.settings.psk.bytes; ++i) {
        if (write.payload.channel.settings.psk.bytes[i] != 0U) {
            any_key_byte = true;
        }
    }
    MESH_TEST_FAIL_IF(any_key_byte, "a cleared slot should carry no key bytes at all");

    /* The verb is answered per slot, not per section: a slot the radio never sent is refused
       exactly as a save of it is, rather than clearing something that is not there. */
    action.channel = 3U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOENT,
                      "a slot the radio never sent cannot be cleared");

    /* And an ordinary save of the same slot is untouched by any of it: the verb travels in
       `number`, so a save - which carries MESH_UI_SETTINGS_ACTION_NONE - still edits. */
    action.channel = 1U;
    action.number = (uint32_t)MESH_UI_SETTINGS_ACTION_NONE;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.payload.channel.role != meshtastic_Channel_Role_SECONDARY ||
                          strcmp(write.payload.channel.settings.name, "Renamed") != 0 ||
                          write.payload.channel.settings.psk.size != 16U ||
                          write.payload.channel.settings.id != 77U,
                      "a plain save of the same slot should still carry the radio's copy");
    record_success(test_name);
}

/* LoRa and Security rows, and the writes built from them. */
MESH_TEST_CASE(app_lora_security_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    radio.has_lora = true;
    radio.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    radio.lora.use_preset = true;
    radio.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    radio.lora.hop_limit = 3U;
    radio.lora.tx_enabled = true;
    radio.lora.frequency_offset = 1.5f;
    radio.has_security = true;
    radio.security.private_key.size = 32U;
    memset(radio.security.private_key.bytes, 0x11, 32U);
    radio.security.public_key.size = 32U;
    memset(radio.security.public_key.bytes, 0x22, 32U);
    radio.security.admin_key_count = 2U;
    radio.security.admin_key[0].size = 32U;
    memset(radio.security.admin_key[0].bytes, 0x33, 32U);
    radio.security.admin_key[1].size = 32U;
    memset(radio.security.admin_key[1].bytes, 0x44, 32U);

    /* The flattened view carries the keys for the rows. */
    struct mesh_ui_settings settings;
    struct mesh_ui_action probe;
    memset(&probe, 0, sizeof probe);
    struct mesh_ui_snapshot *unused = NULL;
    (void)unused;
    (void)probe;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.region = 1U;
    settings.use_preset = true;
    settings.tx_power = 0;
    settings.has_security = true;
    settings.private_key_len = 32U;
    memset(settings.private_key, 0x11, 32U);
    settings.admin_key_count = 2U;
    settings.admin_key_lens[0] = 32U;
    memset(settings.admin_keys[0], 0x33, 32U);
    settings.admin_key_lens[1] = 32U;
    struct mesh_ui_settings_item item;
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_LORA,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 26U ||
            !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
            item.field != MESH_UI_FIELD_LORA_REGION || strcmp(item.value, "US") != 0 ||
            mesh_ui_settings_enum_count(MESH_UI_FIELD_LORA_REGION) != 38U ||
            strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_LORA_REGION, 37U), "ITU2 1.25m") != 0 ||
            !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 8U, &item) ||
            item.field != MESH_UI_FIELD_LORA_TX_POWER || strcmp(item.value, "max") != 0 ||
            mesh_ui_settings_number_step(MESH_UI_FIELD_LORA_TX_POWER, 0U, +1) != 2U ||
            !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 5U, &item) ||
            strcmp(item.value, "4/0") != 0,
        "LoRa rows are wrong");
    MESH_TEST_FAIL_IF(
        mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_SECURITY,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 10U ||
            !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_SECURITY,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
            item.field != MESH_UI_FIELD_SECURITY_PRIVATE_KEY || item.kind != MESH_UI_SETTING_KEY ||
            strlen(item.text) != 44U || strstr(item.value, "256-bit") == NULL ||
            !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_SECURITY,
                                   MESH_UI_SETTINGS_NO_CHANNEL, 4U, &item) ||
            item.field != MESH_UI_FIELD_SECURITY_ADMIN_KEY_2 || strcmp(item.value, "none") != 0 ||
            !mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_LORA) ||
            !mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_SECURITY),
        "Security rows are wrong");

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_LORA;
    action.channel = MESH_UI_SETTINGS_NO_CHANNEL;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_LORA_REGION;
    action.edits[0].number = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    action.edits[1].field = MESH_UI_FIELD_LORA_HOPS;
    action.edits[1].number = 5U;
    action.edits[2].field = MESH_UI_FIELD_LORA_TX_POWER;
    action.edits[2].number = 20U;
    struct mesh_admin_request write;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.kind != MESH_ADMIN_SET_CONFIG ||
                          write.type != meshtastic_AdminMessage_ConfigType_LORA_CONFIG ||
                          write.payload.config.which_payload_variant !=
                              meshtastic_Config_lora_tag ||
                          write.payload.config.payload_variant.lora.region !=
                              meshtastic_Config_LoRaConfig_RegionCode_EU_868 ||
                          write.payload.config.payload_variant.lora.hop_limit != 5U ||
                          write.payload.config.payload_variant.lora.tx_power != 20 ||
                          !write.payload.config.payload_variant.lora.use_preset ||
                          write.payload.config.payload_variant.lora.frequency_offset != 1.5f,
                      "the LoRa write should carry the edits over the radio's copy");

    /* Security: a new private key is clamped and the public key cleared for the firmware to
       derive; clearing admin key 1 compacts the list. */
    action.section = MESH_UI_SETTINGS_SECURITY;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_SECURITY_PRIVATE_KEY;
    action.edits[0].number = MESH_UI_PSK_RANDOM_256;
    action.edits[1].field = MESH_UI_FIELD_SECURITY_ADMIN_KEY_0;
    action.edits[1].number = MESH_UI_PSK_NONE;
    action.edits[2].field = MESH_UI_FIELD_SECURITY_MANAGED;
    action.edits[2].number = 1U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.type != meshtastic_AdminMessage_ConfigType_SECURITY_CONFIG ||
                          write.payload.config.which_payload_variant !=
                              meshtastic_Config_security_tag,
                      "the Security write should build");
    const meshtastic_Config_SecurityConfig *sec = &write.payload.config.payload_variant.security;
    MESH_TEST_FAIL_IF(sec->private_key.size != 32U || (sec->private_key.bytes[0] & 7U) != 0U ||
                          (sec->private_key.bytes[31] & 0x80U) != 0U ||
                          (sec->private_key.bytes[31] & 0x40U) == 0U ||
                          memcmp(sec->private_key.bytes, radio.security.private_key.bytes, 32U) ==
                              0 ||
                          sec->public_key.size != 0U || !sec->is_managed,
                      "a new private key should be clamped and the public key cleared");
    MESH_TEST_FAIL_IF(sec->admin_key_count != 1U || sec->admin_key[0].size != 32U ||
                          sec->admin_key[0].bytes[0] != 0x44U || sec->admin_key[1].size != 0U,
                      "clearing an admin key should compact the list");
    /* Restoring a backed-up private key and adding an admin key by text. */
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_SECURITY_PRIVATE_KEY;
    action.edits[0].number = MESH_UI_PSK_TYPED;
    uint8_t restore[32];
    memset(restore, 0x5A, 32U);
    mesh_ui_settings_key_text(restore, 32U, action.edits[0].text, sizeof action.edits[0].text);
    action.edits[1].field = MESH_UI_FIELD_SECURITY_ADMIN_KEY_2;
    action.edits[1].number = MESH_UI_PSK_TYPED;
    memset(restore, 0x66, 32U);
    mesh_ui_settings_key_text(restore, 32U, action.edits[1].text, sizeof action.edits[1].text);
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          sec->private_key.bytes[0] != 0x5AU || sec->private_key.size != 32U ||
                          sec->public_key.size != 0U || sec->admin_key_count != 3U ||
                          sec->admin_key[2].bytes[0] != 0x66U,
                      "typed keys should be restored as given");
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "AQ=="); /* 1 byte */
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -EINVAL,
                      "a private key must be 32 bytes");
    record_success(test_name);
}

/*
 * With both links available the app must prefer the plugged-in node, route the connect to the
 * serial transport, list USB ports above BLE advertisers, and - the point of the shared session -
 * fold what the USB radio says into the app's own session rather than one buried in the link.
 */
MESH_TEST_CASE(app_link_routing, unit) {
    const char *failure = NULL;
    int pair[2] = {-1, -1};
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    MESH_TEST_FAIL_IF(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0, "socketpair failed");
    (void)fcntl(pair[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pair[1], F_SETFL, O_NONBLOCK);

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:06", .name = "NodeSix", .rssi = -30, .paired = true},
    };
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 1U,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    const struct mesh_serial_device_info ports[] = {mesh_test_serial_device()};
    struct mesh_serial_usb_mock_config serial_mock;
    memset(&serial_mock, 0, sizeof serial_mock);
    serial_mock.devices = ports;
    serial_mock.device_count = 1U;
    serial_mock.bound_path = "/dev/ttyUSB0";
    serial_mock.open_fd = pair[0];
    mesh_serial_usb_mock_enable(&serial_mock);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "link_routing")) {
        failure = "mkdtemp failed";
        goto cleanup;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_transport *serial = mesh_serial_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);
    mesh_serial_transport_refresh_devices(serial);

    /* USB wins even though a BLE advertiser is in range at a healthy RSSI. */
    mesh_app_autoconnect(&app);
    if (!mesh_serial_transport_is_connecting(serial)) {
        failure = "auto-connect should have opened the USB port first";
        goto cleanup;
    }
    if (mesh_ble_transport_connected_address(ble) != NULL ||
        mesh_ble_transport_is_connecting(ble)) {
        failure = "the BLE link should have been left alone";
        goto cleanup;
    }

    mesh_test_serial_sleep_ms(150);
    mesh_transport_registry_tick(&app.transport_registry);
    const char *connected = mesh_app_connected_identifier();
    if (connected == NULL || strcmp(connected, "/dev/ttyUSB0") != 0) {
        failure = "the app should report the tty as the connected radio";
        goto cleanup;
    }
    if (mesh_app_active_transport() != serial) {
        failure = "the serial transport should be the active link";
        goto cleanup;
    }

    /* The radio's reply has to land in the app's session, not one hidden in the transport. */
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = 0x0BADCAFEU;
    uint8_t encoded[256];
    size_t encoded_len = 0U;
    if (!mesh_test_encode_from_radio(&from_radio, encoded, sizeof encoded, &encoded_len)) {
        failure = "failed to encode the reply";
        goto cleanup;
    }
    uint8_t frame[300];
    size_t frame_len = 0U;
    mesh_stream_frame_encode(encoded, encoded_len, frame, sizeof frame, &frame_len);
    if (write(pair[1], frame, frame_len) != (ssize_t)frame_len) {
        failure = "failed to write the reply into the port";
        goto cleanup;
    }
    if (mesh_serial_transport_pump(serial) <= 0) {
        failure = "pump should have read the reply";
        goto cleanup;
    }
    if (!app.session.handshake.has_my_info ||
        app.session.handshake.my_info.my_node_num != 0x0BADCAFEU) {
        failure = "the USB link should feed the app's own session";
        goto cleanup;
    }

    /* The Devices tab shows one list: USB ports first, then BLE advertisers. */
    mesh_app_publish_ui_state(&app);
    if (app.ui_store.device_count != 2U) {
        failure = "both the USB port and the BLE advertiser should be listed";
        goto cleanup;
    }
    if (app.ui_store.devices[0].kind != (uint8_t)MESH_UI_DEVICE_SERIAL ||
        strcmp(app.ui_store.devices[0].identifier, "/dev/ttyUSB0") != 0 ||
        !app.ui_store.devices[0].connected) {
        failure = "the connected USB port should be the first row";
        goto cleanup;
    }
    if (app.ui_store.devices[1].kind != (uint8_t)MESH_UI_DEVICE_BLE ||
        strcmp(app.ui_store.devices[1].identifier, "AA:BB:CC:DD:EE:06") != 0 ||
        app.ui_store.devices[1].connected) {
        failure = "the BLE advertiser should follow it, unconnected";
        goto cleanup;
    }

    /* Nothing should be reconnected while a link is up. */
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (mesh_ble_transport_is_connecting(ble) ||
        mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "auto-connect should stay put while the USB link is up";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    mesh_serial_usb_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    if (pair[0] >= 0) {
        close(pair[0]);
    }
    if (pair[1] >= 0) {
        close(pair[1]);
    }
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A release that exists in the index but has published no assets.
 *
 * This is the one refusal `firmware_blocker()` cannot phrase. It is computed from the board and
 * the bus, and both are fine here - the board resolved, it is actively supported, and it is on
 * the bus its own path names - so the blocker is NONE and has no line to offer. What is missing
 * is the release's manifest, which `fw_can_install` asks for separately because an index entry
 * can appear before its assets do; ordinary on the alpha channel in the minutes after a publish,
 * and the state tests/suites/firmware_catalog.c's newest alpha fixture is in.
 *
 * Unstated, that combination reaches Settings > About radio as a newer version with no install
 * press and nothing saying why - the section's whole job, missed in the one case where nothing
 * is actually wrong. It cannot move into the blocker, because firmware.c recomputes that before
 * it sets the state, so a state-dependent answer there would be computed against the old one.
 */
MESH_TEST_CASE(app_assetless_release_says_why_it_cannot_install, unit) {
    const char *failure = NULL;
    int pair[2] = {-1, -1};
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    MESH_TEST_FAIL_IF(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0, "socketpair failed");
    (void)fcntl(pair[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pair[1], F_SETFL, O_NONBLOCK);

    struct mesh_bluez_mock_config mock_config = {.adapter_path = "/org/bluez/hci0"};
    mesh_bluez_client_mock_enable(&mock_config);

    const struct mesh_serial_device_info ports[] = {mesh_test_serial_device()};
    struct mesh_serial_usb_mock_config serial_mock;
    memset(&serial_mock, 0, sizeof serial_mock);
    serial_mock.devices = ports;
    serial_mock.device_count = 1U;
    serial_mock.bound_path = "/dev/ttyUSB0";
    serial_mock.open_fd = pair[0];
    mesh_serial_usb_mock_enable(&serial_mock);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "assetless")) {
        failure = "mkdtemp failed";
        goto cleanup;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *serial = mesh_serial_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_serial_transport_refresh_devices(serial);
    mesh_app_autoconnect(&app);
    mesh_test_serial_sleep_ms(150);
    mesh_transport_registry_tick(&app.transport_registry);
    if (mesh_app_connected_identifier() == NULL) {
        failure = "the USB port should be the connected radio";
        goto cleanup;
    }

    /*
     * A check that found something, on a board that could take it, over the bus that board is
     * flashed on - and a release with no manifest behind it. No metadata is fed to the session,
     * so mesh_app_flatten_firmware()'s radio-swap guard reads a model of 0 and leaves this
     * answer alone.
     */
    app.firmware.state = MESH_FIRMWARE_AVAILABLE;
    app.firmware.blocker = MESH_FIRMWARE_BLOCKER_NONE;
    app.firmware.boards.count = 1U;
    app.firmware.boards.found = 1U;
    app.firmware.boards.entries[0].actively_supported = true;
    app.firmware.boards.entries[0].path = MESH_FIRMWARE_PATH_USB;
    snprintf(app.firmware.boards.entries[0].target, sizeof app.firmware.boards.entries[0].target,
             "%s", "heltec-mesh-node-t114");
    snprintf(app.firmware.release.version, sizeof app.firmware.release.version, "%s",
             "2.8.0.47db0e3");
    app.firmware.release.manifest_url[0] = '\0';

    mesh_app_publish_ui_state(&app);
    if (app.ui_store.settings.fw_can_install) {
        failure = "a release with no manifest has nothing to install";
        goto cleanup;
    }
    if (app.ui_store.settings.fw_blocker_reason[0] == '\0') {
        failure = "a newer release that cannot be installed must say why";
        goto cleanup;
    }

    /* And with the manifest present the refusal goes away again, so the reason is answering the
       missing asset rather than standing in for every empty blocker. */
    snprintf(app.firmware.release.manifest_url, sizeof app.firmware.release.manifest_url, "%s",
             "https://example.invalid/firmware-2.8.0.47db0e3.json");
    mesh_app_publish_ui_state(&app);
    if (app.ui_store.settings.fw_blocker_reason[0] != '\0') {
        failure = "a release with its manifest is not refused";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    mesh_serial_usb_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    if (pair[0] >= 0) {
        close(pair[0]);
    }
    if (pair[1] >= 0) {
        close(pair[1]);
    }
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A radio that does not come back after its own update has its bond dropped for it.
 *
 * Installing firmware is the one thing this client does that changes a radio's half of a BLE
 * bond, and on an ESP32 it does: after 2.8.0.47db0e3 a Heltec V3 refused every connect with
 * le-connection-abort-by-local, because the LTK here was one the radio no longer had. Nothing
 * in the link layer says so - BlueZ reports the device Paired, since our half is intact - so
 * mesh_ble_do_connect() takes the bonded branch at ble_transport.c and the client can only
 * answer "connect failed". Forgetting the node by hand is the way out, and expecting a user to
 * work that out from two words is the bug.
 *
 * Both directions are the case, because dropping the bond every time is the other bug: an
 * upgrade whose keys survived would charge a PIN nobody needed to type.
 */
MESH_TEST_CASE(app_drops_a_bond_its_own_update_invalidated, unit) {
    const char *failure = NULL;
    bool app_ready = false;
    unsigned removed = 0U;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    struct mesh_bluez_mock_config mock_config = {.adapter_path = "/org/bluez/hci0",
                                                 .remove_device_calls = &removed};
    mesh_bluez_client_mock_enable(&mock_config);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "bondwatch")) {
        failure = "mkdtemp failed";
        goto cleanup;
    }
    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;
    /* The bond lives on the adapter, so the transport has to be up for one to be dropped. */
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }

    struct mesh_firmware_update update;
    memset(&update, 0, sizeof update);
    update.path = MESH_FIRMWARE_PATH_BLE;
    snprintf(update.where, sizeof update.where, "%s", "9C:13:9E:9D:0A:D9");

    /* Nothing is watched until an install finishes. */
    if (mesh_app_firmware_settle_bond(&app, NULL, 1000U)) {
        failure = "a client that has flashed nothing drops no bonds";
        goto cleanup;
    }

    /* A cable leaves no bond behind, so the USB path is not watched at all. */
    struct mesh_firmware_update over_usb = update;
    over_usb.path = MESH_FIRMWARE_PATH_USB;
    mesh_app_firmware_watch_bond(&app, &over_usb, 1000U);
    if (app.firmware_bond_watch[0] != '\0') {
        failure = "a serial radio has no bond to lose";
        goto cleanup;
    }

    /*
     * And the wiring: a *finished* install arms it, rather than the watch only working when
     * something calls it by hand. This is the install's own completion, the one
     * mesh_firmware_update_start() is handed.
     */
    struct mesh_firmware_update finished = update;
    finished.state = MESH_FIRMWARE_UPDATE_DONE;
    snprintf(finished.release.version, sizeof finished.release.version, "%s", "2.8.0.47db0e3");
    mesh_app_firmware_update_done(&app, &finished);
    if (app.firmware_bond_watch[0] == '\0') {
        failure = "a finished BLE install watches the radio it flashed";
        goto cleanup;
    }
    /* A failed one does not: the radio is running what it was, and its bond with it. */
    app.firmware_bond_watch[0] = '\0';
    struct mesh_firmware_update refused = finished;
    refused.state = MESH_FIRMWARE_UPDATE_FAILED;
    mesh_app_firmware_update_done(&app, &refused);
    if (app.firmware_bond_watch[0] != '\0') {
        failure = "an install that failed changed no firmware and no keys";
        goto cleanup;
    }
    mesh_app_firmware_update_done(&app, &finished);
    if (mesh_app_firmware_settle_bond(&app, "9C:13:9E:9D:0A:D9", 2000U)) {
        failure = "a radio back on the link kept its bond";
        goto cleanup;
    }
    if (app.firmware_bond_watch[0] != '\0' || removed != 0U) {
        failure = "and the watch ends without touching it";
        goto cleanup;
    }

    /*
     * A different radio holding the link is not a verdict on this one.
     *
     * mesh_app_autoconnect() returns early whenever a link is up - one radio at a time - so a
     * serial node plugged in after the update means BLE is never reached for, and the watched
     * address could not match however long this waited. Expiring against that would drop a bond
     * nothing had found fault with. The clock has to wait for the bus to be free.
     */
    mesh_app_firmware_watch_bond(&app, &update, 1000U);
    if (mesh_app_firmware_settle_bond(&app, "/dev/ttyUSB0", 1000U + 600000U) || removed != 0U) {
        failure = "another radio on the link is not this radio failing to come back";
        goto cleanup;
    }
    if (app.firmware_bond_watch[0] == '\0') {
        failure = "and the watch is still pending, not settled by the wrong radio";
        goto cleanup;
    }
    /* And once that link goes, the grace starts from there rather than from the install. */
    if (mesh_app_firmware_settle_bond(&app, NULL, 1000U + 600000U + 30000U) || removed != 0U) {
        failure = "the grace runs from when the bus was free, not from the install";
        goto cleanup;
    }
    if (!mesh_app_firmware_settle_bond(&app, NULL, 1000U + 600000U + 61000U) || removed != 1U) {
        failure = "and then a radio that never came back loses its bond";
        goto cleanup;
    }
    removed = 0U;

    /*
     * A removal that failed for a reason that can pass keeps the watch.
     *
     * An adapter still coming back from the install answers NotReady, and an adapter that is
     * away has not lost the bond it persisted - so giving up on one would leave the stale key
     * in place and the reconnects failing, which is the state this exists to end.
     */
    mock_config.remove_device_result = -ENOTCONN;
    mesh_bluez_client_mock_enable(&mock_config);
    mesh_app_firmware_watch_bond(&app, &update, 1000U);
    if (mesh_app_firmware_settle_bond(&app, NULL, 1000U + 61000U)) {
        failure = "a removal that did not happen is not a bond dropped";
        goto cleanup;
    }
    /* The counter is attempts that reached the adapter, so this says the removal was really
       tried rather than skipped on the way to being retried. */
    if (removed != 1U) {
        failure = "a removal that failed is still a removal that was attempted";
        goto cleanup;
    }
    if (app.firmware_bond_watch[0] == '\0') {
        failure = "and the watch survives it, so the removal is tried again";
        goto cleanup;
    }
    /* Once the adapter is back, the retry lands. */
    removed = 0U;
    mock_config.remove_device_result = 0;
    mesh_bluez_client_mock_enable(&mock_config);
    if (!mesh_app_firmware_settle_bond(&app, NULL, 1000U + 61000U + 6000U) || removed != 1U) {
        failure = "the retry drops the bond once the adapter answers";
        goto cleanup;
    }
    removed = 0U;

    /*
     * And a bond that is already gone settles rather than retrying for ever: DoesNotExist is
     * the state this was trying to reach, not a failure to reach it.
     */
    mock_config.remove_device_result = -ENOENT;
    mesh_bluez_client_mock_enable(&mock_config);
    mesh_app_firmware_watch_bond(&app, &update, 1000U);
    if (mesh_app_firmware_settle_bond(&app, NULL, 1000U + 61000U)) {
        failure = "a bond that was already gone is not one this dropped";
        goto cleanup;
    }
    if (app.firmware_bond_watch[0] != '\0') {
        failure = "but it is settled, not retried for ever";
        goto cleanup;
    }
    mock_config.remove_device_result = 0;
    mesh_bluez_client_mock_enable(&mock_config);
    removed = 0U;

    /* The radio that did not come back: silent past the grace, bond dropped once. */
    mesh_app_firmware_watch_bond(&app, &update, 1000U);
    if (mesh_app_firmware_settle_bond(&app, NULL, 2000U)) {
        failure = "the grace is a grace, not an instant verdict";
        goto cleanup;
    }
    if (removed != 0U) {
        failure = "and nothing is dropped inside it";
        goto cleanup;
    }
    if (!mesh_app_firmware_settle_bond(&app, NULL, 1000U + 60000U)) {
        failure = "a radio that never came back has a bond this client can see is dead";
        goto cleanup;
    }
    if (removed != 1U) {
        failure = "which is dropped, so the next connect pairs instead of failing";
        goto cleanup;
    }
    /* Once. A watch left armed would drop the bond the user is in the middle of making. */
    if (app.firmware_bond_watch[0] != '\0' ||
        mesh_app_firmware_settle_bond(&app, NULL, 1000U + 120000U) || removed != 1U) {
        failure = "and dropped once, not on every turn after";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(app_takes_short_turns_while_a_handover_has_the_radio, unit) {
    struct mesh_app app;
    memset(&app, 0, sizeof app);
    app.config = mesh_app_config_default();

    MESH_TEST_FAIL_IF(mesh_app_turn_ms(NULL) != 0, "no app is no wait at all");
    MESH_TEST_FAIL_IF(app.config.idle_timeout_ms != 1000,
                      "the default turn is the configured idle timeout");
    MESH_TEST_FAIL_IF(mesh_app_turn_ms(&app) != app.config.idle_timeout_ms,
                      "an idle client waits the timeout it was configured with");

    /* Every rung that holds the radio, on the bus that pays for a slow tick. */
    static const enum mesh_firmware_install_state k_usb[] = {
        MESH_FIRMWARE_INSTALL_ARMING, MESH_FIRMWARE_INSTALL_WAITING, MESH_FIRMWARE_INSTALL_WRITING,
        MESH_FIRMWARE_INSTALL_RESTARTING};
    for (size_t i = 0; i < sizeof k_usb / sizeof k_usb[0]; ++i) {
        app.firmware_update.usb.state = k_usb[i];
        MESH_TEST_FAIL_IF(!mesh_firmware_update_holds_the_radio(&app.firmware_update),
                          "every one of these holds the radio");
        MESH_TEST_FAIL_IF(mesh_app_turn_ms(&app) >= app.config.idle_timeout_ms,
                          "and a turn while it does is shorter than an idle one");
    }
    app.firmware_update.usb.state = MESH_FIRMWARE_INSTALL_IDLE;

    static const enum mesh_firmware_ota_state k_ble[] = {
        MESH_FIRMWARE_OTA_ARMING, MESH_FIRMWARE_OTA_WAITING, MESH_FIRMWARE_OTA_CONNECTING,
        MESH_FIRMWARE_OTA_SENDING, MESH_FIRMWARE_OTA_RESTARTING};
    for (size_t i = 0; i < sizeof k_ble / sizeof k_ble[0]; ++i) {
        app.firmware_update.ble.state = k_ble[i];
        MESH_TEST_FAIL_IF(!mesh_firmware_update_holds_the_radio(&app.firmware_update),
                          "the same on the bus the transfer runs on");
        MESH_TEST_FAIL_IF(mesh_app_turn_ms(&app) >= app.config.idle_timeout_ms,
                          "which is the one that was taking 74 minutes");
    }

    /* The chunk has to fit in the turn or the turn buys nothing: at 512 bytes an ACK, anything
       near a second is the bug this case is about. */
    app.firmware_update.ble.state = MESH_FIRMWARE_OTA_SENDING;
    MESH_TEST_FAIL_IF(mesh_app_turn_ms(&app) > 50,
                      "a transfer turn is a link round trip, not a tick");

    /* And it is a ceiling rather than a setting: a client already asking for faster turns than
       the transfer needs keeps its own, and 0 stays the drain-and-return it means elsewhere. */
    app.config.idle_timeout_ms = 5;
    MESH_TEST_FAIL_IF(mesh_app_turn_ms(&app) != 5, "a shorter configured turn is left alone");
    app.config.idle_timeout_ms = 0;
    MESH_TEST_FAIL_IF(mesh_app_turn_ms(&app) != 0, "and a zero turn still drains and returns");
    /* The other direction: an unbounded wait has no deadline to come back from, so a transfer
       is the one case where the bound is imposed rather than lowered. */
    app.config.idle_timeout_ms = -1;
    MESH_TEST_FAIL_IF(mesh_app_turn_ms(&app) <= 0 || mesh_app_turn_ms(&app) > 50,
                      "an unbounded turn is bounded while a transfer needs the tick");

    app.firmware_update.ble.state = MESH_FIRMWARE_OTA_IDLE;
    app.config.idle_timeout_ms = 1000;
    MESH_TEST_FAIL_IF(mesh_app_turn_ms(&app) != 1000,
                      "and the turn goes back to the idle one when the job lets go");
    record_success(test_name);
}

/*
 * The arm that told the install it had been refused, having just armed the radio.
 *
 * The two modules either side of this hook count in different units. A session answers "how
 * many admin requests did I queue", so a verb that went out is 1; the install's hook answers
 * "0, or -errno", so 1 is a refusal. The USB arm handed the count straight across, which meant
 * every successful DFU request was read as the radio turning it down: the job stopped at
 * "waiting for radio -> failed" one second after the image was staged, the client said the
 * radio would not take the request, and the T114 - which had taken it - sat in its bootloader
 * with nothing on the way. The BLE arm next to it had always translated, which is the asymmetry
 * this case exists to stop coming back.
 *
 * Both halves are asserted, because the return alone is green against a hook that answered 0
 * by queueing nothing at all - which is the same install stopped one rung further on.
 */
MESH_TEST_CASE(app_firmware_arm_reports_a_queued_verb_as_armed, unit) {
    const char *failure = NULL;
    int pair[2] = {-1, -1};
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    MESH_TEST_FAIL_IF(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0, "socketpair failed");
    (void)fcntl(pair[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pair[1], F_SETFL, O_NONBLOCK);

    struct mesh_bluez_mock_config mock_config = {.adapter_path = "/org/bluez/hci0"};
    mesh_bluez_client_mock_enable(&mock_config);

    const struct mesh_serial_device_info ports[] = {mesh_test_serial_device()};
    struct mesh_serial_usb_mock_config serial_mock;
    memset(&serial_mock, 0, sizeof serial_mock);
    serial_mock.devices = ports;
    serial_mock.device_count = 1U;
    serial_mock.bound_path = "/dev/ttyUSB0";
    serial_mock.open_fd = pair[0];
    mesh_serial_usb_mock_enable(&serial_mock);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "armusb")) {
        failure = "mkdtemp failed";
        goto cleanup;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_serial_transport_refresh_devices(mesh_serial_transport());
    mesh_app_autoconnect(&app);
    mesh_test_serial_sleep_ms(150);
    mesh_transport_registry_tick(&app.transport_registry);
    if (mesh_app_connected_identifier() == NULL) {
        failure = "the USB port should be the connected radio";
        goto cleanup;
    }
    /*
     * The refusal first, while the handshake has no my_info to address a verb to: with nothing
     * to queue on the hook owes the install an errno rather than a zero it would act on.
     */
    const struct mesh_firmware_update_hooks hooks = mesh_app_firmware_hooks(&app);
    if (hooks.arm_usb == NULL || hooks.userdata != &app) {
        failure = "the press hands over an arm and the app behind it";
        goto cleanup;
    }
    if (hooks.arm_usb(hooks.userdata) >= 0) {
        failure = "an arm with nothing to address should fail, and say so as -errno";
        goto cleanup;
    }

    /* An admin verb needs somewhere to be addressed, which is what my_info is for. */
    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = 0x0BADCAFEU;
    if (!mesh_test_session_feed_from_radio(&app.session, &my_info)) {
        failure = "the session should take my_info";
        goto cleanup;
    }

    /*
     * And now the arm that works. This is the call that queues the verb - the session answers
     * it with the count, 1 - so it is the one that returned 1 into an install that reads any
     * non-zero as "the radio would not take it".
     */
    const size_t before = app.session.settings.queue_len;
    if (hooks.arm_usb(hooks.userdata) != 0) {
        failure = "an arm that queued the verb is an arm that worked";
        goto cleanup;
    }
    /* And it queued: a hook that answered 0 by doing nothing would be the same bug the other
       way round, with a radio that never hears the verb and an install that waits for a
       bootloader until it times out. */
    if (app.session.settings.queue_len <= before) {
        failure = "and the verb it reported is on the queue";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    mesh_serial_usb_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    if (pair[0] >= 0) {
        close(pair[0]);
    }
    if (pair[1] >= 0) {
        close(pair[1]);
    }
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A BLE connect can return 0 and still fail seconds later, when BlueZ finishes service discovery
 * and StartNotify is rejected because the node was never paired. That used to leave the UI stuck
 * on "connecting" with the reason only in the log.
 */
MESH_TEST_CASE(app_connect_failure_toast, unit) {
    const char *failure = NULL;
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:07", .name = "NodeSeven", .rssi = -40, .paired = true},
    };
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 1U,
        /* The node answers Connect and resolves services, then refuses the subscription. */
        .connect_pending_polls = 1U,
        .subscribe_result = -EACCES,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "connect_fail")) {
        failure = "mkdtemp failed";
        goto cleanup;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_serial = false;

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);

    if (app.ui_controller.on_action == NULL) {
        failure = "the app should have installed a UI action handler";
        goto cleanup;
    }

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_CONNECT;
    action.kind = (uint8_t)MESH_UI_DEVICE_BLE;
    snprintf(action.identifier, sizeof action.identifier, "%s", "AA:BB:CC:DD:EE:07");
    app.ui_controller.on_action(app.ui_controller.action_userdata, &action);

    /* Connect has only been sent; nothing has failed yet. */
    if (!mesh_ble_transport_is_connecting(ble)) {
        failure = "the connect should be in flight";
        goto cleanup;
    }
    if (strstr(app.ui_store.nav.toast, "Connecting") == NULL) {
        failure = "the user should first be told the connect is in flight";
        goto cleanup;
    }
    if (mesh_app_report_link_errors(&app)) {
        failure = "no failure should be reported while the connect is still pending";
        goto cleanup;
    }

    /* Now the reply lands, services resolve, and StartNotify is refused. */
    mesh_transport_registry_tick(&app.transport_registry);
    if (mesh_ble_transport_is_connecting(ble) ||
        mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "the link should have been dropped";
        goto cleanup;
    }

    if (!mesh_app_report_link_errors(&app)) {
        failure = "the pairing failure should have been drained";
        goto cleanup;
    }
    if (strstr(app.ui_store.nav.toast, "pairing") == NULL ||
        strstr(app.ui_store.nav.toast, "EE:07") == NULL) {
        failure = "the pairing failure should have reached the screen";
        goto cleanup;
    }

    /* One report per attempt: the same failure must not keep re-toasting every turn. */
    mesh_ui_store_set_toast(&app.ui_store, test_now_ms(), "quiet");
    if (mesh_app_report_link_errors(&app)) {
        failure = "the failure should be reported once, not on every turn";
        goto cleanup;
    }

    /*
     * A connect that returned 0 and failed later is still a failure. Nothing else tells
     * auto-connect that, so without it the backoff never grows and a node that refuses every
     * time is retried every couple of seconds forever.
     */
    if (app.autoconnect_failures == 0U) {
        failure = "the late failure should have counted against auto-connect";
        goto cleanup;
    }
    const unsigned failures_before = app.autoconnect_failures;
    const uint64_t retry_before = app.autoconnect_retry_at_ms;

    /* Auto-connect retries the same doomed node on every backoff, so its failures stay in the
       log; only a connect the user asked for is worth interrupting them for. */
    app.autoconnect_retry_at_ms = 0U;
    snprintf(app.config.preferred_ble_device, sizeof app.config.preferred_ble_device, "%s",
             "AA:BB:CC:DD:EE:07");
    mesh_app_autoconnect(&app);
    mesh_transport_registry_tick(&app.transport_registry);
    if (!mesh_app_report_link_errors(&app)) {
        failure = "the auto-connect failure should still have been drained";
        goto cleanup;
    }
    if (strcmp(app.ui_store.nav.toast, "quiet") != 0) {
        failure = "an auto-connect failure should not raise a toast";
        goto cleanup;
    }
    if (app.autoconnect_failures <= failures_before) {
        failure = "each failed attempt should push the backoff out further";
        goto cleanup;
    }
    (void)retry_before;

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Who survives the Nodes tab's budget, and specifically what happens on the day you move the
 * Brick from one of your radios to another: the one you unplugged has no pin on the new radio
 * (favorites are NodeDB state, per receiver) and must not sink to the bottom of a busy mesh.
 */
MESH_TEST_CASE(app_node_rank_known_radio, unit) {
    const uint32_t abc = 0xABC123U;
    const uint32_t def = 0xDEF456U;
    const uint32_t peer = 0x00777U;
    const uint32_t stranger = 0x00888U;

    struct mesh_ui_preferences prefs;
    memset(&prefs, 0, sizeof prefs);

    struct mesh_message_log log;
    mesh_message_log_reset(&log);
    log.count = 1U;
    log.entries[0].from = peer;
    log.entries[0].to = abc;

    struct mesh_node_summary nodes[4];
    memset(nodes, 0, sizeof nodes);
    nodes[0].node_id = abc;
    nodes[1].node_id = def;
    nodes[2].node_id = peer;
    nodes[3].node_id = stranger;
    nodes[3].via_mqtt = true;

    /* Connected to ABC123, with DEF456 pinned into ABC123's NodeDB. */
    prefs.known_radio_count = 1U;
    prefs.known_radios[0] = abc;
    nodes[1].is_favorite = true;
    if (mesh_app_node_rank(&nodes[0], abc, &log, &prefs) != 0U ||
        mesh_app_node_rank(&nodes[1], abc, &log, &prefs) != 1U ||
        mesh_app_node_rank(&nodes[2], abc, &log, &prefs) != 3U ||
        mesh_app_node_rank(&nodes[3], abc, &log, &prefs) != 5U) {
        record_failure(test_name, "us, then pinned, then a message peer, then MQTT");
        return;
    }

    /* Now the Brick is moved onto DEF456. Its NodeDB never heard of the pin ABC123 carried,
       so the flag is gone - and ABC123 is nobody's favorite over here. */
    mesh_ui_preferences_note_radio(&prefs, def);
    nodes[1].is_favorite = false;
    MESH_TEST_FAIL_IF(mesh_app_node_rank(&nodes[1], def, &log, &prefs) != 0U,
                      "the radio we are now on is us, pinned or not");
    MESH_TEST_FAIL_IF(mesh_app_node_rank(&nodes[0], def, &log, &prefs) != 2U,
                      "the radio we just unplugged should rank as one of ours");
    MESH_TEST_FAIL_IF(mesh_app_node_rank(&nodes[2], def, &log, &prefs) != 3U ||
                          mesh_app_node_rank(&nodes[3], def, &log, &prefs) != 5U,
                      "everyone else should keep their tier");

    /* Without the memory - a fresh install, or a radio we have never connected to - ABC123 is
       an ordinary node heard over RF, which is the behaviour this tier exists to avoid. */
    struct mesh_ui_preferences empty;
    memset(&empty, 0, sizeof empty);
    MESH_TEST_FAIL_IF(mesh_app_node_rank(&nodes[0], def, &log, &empty) != 3U ||
                          mesh_app_node_rank(&nodes[0], def, NULL, &empty) != 4U ||
                          mesh_app_node_rank(&nodes[0], def, NULL, NULL) != 4U,
                      "an unknown radio should fall back to the ordinary tiers");

    record_success(test_name);
}

/*
 * The theme switcher, from the button press to the file on disk.
 *
 * The interesting part is that no layer here knows about any other: the row raises an action,
 * the app changes what it is drawing with and writes it down, and the backends find out through
 * the next snapshot. This case walks that whole path with the real store, the real nav and the
 * real controller - only the backend is the stub, because there is no framebuffer in CI.
 */
MESH_TEST_CASE(app_theme_switcher, unit) {
    const char *failure = NULL;
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "theme")) {
        record_failure(test_name, "mkdtemp failed");
        return;
    }
    unsetenv("MESHCLIENT_THEME");

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_serial = false;
    config.enable_ble = false;

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    /* Nothing saved and nothing in the environment: the theme the device has always drawn. */
    const struct mesh_ui_theme *const first = mesh_ui_theme_default();
    if (app.ui_theme != first || app.ui_theme_from_env) {
        failure = "a fresh install should start on the default theme, unpinned";
        goto cleanup;
    }

    /* It reaches the backends as published state rather than through a call of its own. */
    mesh_app_publish_ui_state(&app);
    if (strcmp(app.ui_store.settings.client.theme, first->id) != 0 ||
        strcmp(app.ui_store.settings.client.theme_name, first->name) != 0 ||
        app.ui_store.settings.client.theme_from_env) {
        failure = "the published client info should name the theme in use";
        goto cleanup;
    }

    /* Settings > About, then down to the theme row, exactly as thumbs would. */
    /* The right shoulder to the Settings tab, whatever is between it and Messages. The shoulder
       rather than Right because Right is only the tab switch on a row with no control on it, and
       the Nodes tab in between lands on its filter row. */
    for (unsigned guard = 0; guard <= (unsigned)MESH_UI_SCREEN_COUNT &&
                             app.ui_store.nav.screen != MESH_UI_SCREEN_SETTINGS;
         ++guard) {
        mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_R1);
    }
    mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_A);
    if (app.ui_store.nav.settings_section != MESH_UI_SETTINGS_ABOUT) {
        failure = "A on the first Settings row should open About";
        goto cleanup;
    }

    const uint32_t rows =
        mesh_ui_nav_row_count(&app.ui_store.nav, &app.ui_store, MESH_UI_SCREEN_SETTINGS);
    uint32_t theme_row = rows;
    struct mesh_ui_settings_item item;
    for (uint32_t i = 0; i < rows; ++i) {
        if (mesh_ui_settings_item(&app.ui_store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_THEME) {
            theme_row = i;
        }
    }
    if (theme_row >= rows) {
        failure = "About should offer the theme row";
        goto cleanup;
    }
    for (uint32_t i = 0; i < theme_row; ++i) {
        mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_DOWN);
    }

    /* One press steps to the next theme, remembers it, and says so. */
    mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_A);
    const struct mesh_ui_theme *const second = mesh_ui_theme_next(first);
    if (app.ui_theme != second) {
        failure = "A on the theme row should step to the next theme";
        goto cleanup;
    }
    if (strcmp(app.ui_preferences.theme, second->id) != 0) {
        failure = "the new theme should be recorded in the preferences";
        goto cleanup;
    }
    /* The publish the press does also writes the file, so the choice survives a battery pull
       between the press and the next thing that would have saved it. */
    struct mesh_ui_preferences after_press;
    if (mesh_ui_preferences_load(&after_press, app.ui_preferences_path) != 0 ||
        mesh_ui_theme_resolve(after_press.theme) != second) {
        failure = "the press should have written the new theme to disk";
        goto cleanup;
    }
    if (app.ui_store.nav.toast[0] == '\0') {
        failure = "the press should say which theme it landed on";
        goto cleanup;
    }
    /*
     * Deliberately without publishing first. The press happens inside the event loop and the
     * toast queues a redraw the same turn drains, so the new theme has to be in the published
     * client info by the time this handler returns - otherwise the press's own frame draws the
     * new toast in the old theme and the switch lands a turn later.
     */
    if (strcmp(app.ui_store.settings.client.theme, second->id) != 0 ||
        strcmp(app.ui_store.settings.client.theme_name, second->name) != 0) {
        failure = "the press should publish the new theme before it returns";
        goto cleanup;
    }

    /* All the way round and back to where it started, which is how somebody who has stepped
       into an unreadable theme gets home. */
    for (size_t i = 1; i < mesh_ui_theme_count(); ++i) {
        mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_A);
    }
    if (app.ui_theme != first) {
        failure = "cycling all the way round should come back to the first theme";
        goto cleanup;
    }

    /* And what is on disk keeps up: the file names whatever the last press landed on, which
       is what the next run reads back. */
    struct mesh_ui_preferences reloaded;
    if (mesh_ui_preferences_load(&reloaded, app.ui_preferences_path) != 0 ||
        mesh_ui_theme_resolve(reloaded.theme) != first) {
        failure = "the saved theme should follow the last press";
        goto cleanup;
    }

    /* The language follows the same controller-to-disk path, rebuilding text immediately. */
    uint32_t language_row = rows;
    for (uint32_t i = 0; i < rows; ++i) {
        if (mesh_ui_settings_item(&app.ui_store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.kind == MESH_UI_SETTING_ACTION &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_LANGUAGE) {
            language_row = i;
        }
        mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_UP);
    }
    if (language_row >= rows) {
        failure = "About should offer the language action";
        goto cleanup;
    }
    for (uint32_t i = 0; i < language_row; ++i) {
        mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_DOWN);
    }
    mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_A);
    if (strcmp(mesh_i18n_locale()->id, "es") != 0 ||
        strcmp(app.ui_store.settings.client.language_name, "Español") != 0 ||
        mesh_ui_preferences_load(&reloaded, app.ui_preferences_path) != 0 ||
        strcmp(reloaded.language, "es") != 0) {
        failure = "the language press must select, publish and persist Spanish";
        goto cleanup;
    }
    mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_A);
    if (strcmp(mesh_i18n_locale()->id, "en") != 0) {
        failure = "the language picker must wrap back to English";
        goto cleanup;
    }
    setenv("MESHCLIENT_LANG", "es", 1);
    mesh_i18n_init_with_preference(reloaded.language);
    mesh_app_publish_ui_state(&app);
    if (!mesh_ui_settings_item(&app.ui_store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                               MESH_UI_SETTINGS_NO_CHANNEL, language_row, &item) ||
        item.kind != MESH_UI_SETTING_INFO) {
        failure = "an explicit language override must make the row read-only";
        goto cleanup;
    }
    mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_A);
    if (strcmp(mesh_i18n_locale()->id, "es") != 0) {
        failure = "a read-only language row must not change the language";
    }

cleanup:
    unsetenv("MESHCLIENT_LANG");
    (void)mesh_i18n_set_locale("en");
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    unsetenv("MESHCLIENT_UI_BACKEND");
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * MESHCLIENT_THEME is the deliberate override, so it wins over the saved choice and the row
 * stops offering a press - a switch that sprang back on the next frame would look broken.
 */
MESH_TEST_CASE(app_theme_environment_pin, unit) {
    const char *failure = NULL;
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "theme_env")) {
        record_failure(test_name, "mkdtemp failed");
        return;
    }

    /* A saved choice that the environment is about to overrule. */
    struct mesh_ui_preferences saved;
    memset(&saved, 0, sizeof saved);
    snprintf(saved.theme, sizeof saved.theme, "%s", "colorblind");
    char prefs_path[256];
    snprintf(prefs_path, sizeof prefs_path, "%s/.meshclient/ui_prefs", home_dir);
    if (mesh_ui_preferences_save(&saved, prefs_path) != 0) {
        record_failure(test_name, "could not write the preferences file");
        return;
    }

    setenv("MESHCLIENT_THEME", "light", 1);

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_serial = false;
    config.enable_ble = false;

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    if (app.ui_theme != mesh_ui_theme_by_id("light") || !app.ui_theme_from_env) {
        failure = "the environment should win over the saved theme";
        goto cleanup;
    }

    mesh_app_publish_ui_state(&app);
    if (!app.ui_store.settings.client.theme_from_env) {
        failure = "the published client info should say the environment is holding it";
        goto cleanup;
    }

    /* The row is a fact now, so there is no press to make and the saved choice is untouched. */
    const uint32_t rows =
        mesh_ui_nav_row_count(&app.ui_store.nav, &app.ui_store, MESH_UI_SCREEN_SETTINGS);
    struct mesh_ui_settings_item item;
    for (uint32_t i = 0; i < rows; ++i) {
        if (mesh_ui_settings_item(&app.ui_store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_ABOUT,
                                  MESH_UI_SETTINGS_NO_CHANNEL, i, &item) &&
            item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_THEME) {
            failure = "an environment-held theme should offer no press";
            goto cleanup;
        }
    }
    if (strcmp(app.ui_preferences.theme, "colorblind") != 0) {
        failure = "the environment should not overwrite what the user picked";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    unsetenv("MESHCLIENT_THEME");
    unsetenv("MESHCLIENT_UI_BACKEND");
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Deleting a conversation from the Messages tab, all the way through.
 *
 * The point of the test is the part that is easy to get wrong: a message lives in three places
 * at once - the transport's ring, the history the app read back from its cache at startup, and
 * the store the backends draw - and a delete that reaches only the store survives about a
 * second, because the very next publish rebuilds the store from the other two.
 */
MESH_TEST_CASE(app_delete_conversation, unit) {
    const char *failure = NULL;
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "delete")) {
        record_failure(test_name, "mkdtemp failed");
        return;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_serial = false;
    config.enable_ble = false;

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    /* One broadcast on the primary channel and two halves of a direct exchange, in both the
       store and the cached history - which is the shape the app is in after a restart. */
    struct mesh_ui_message_list messages;
    memset(&messages, 0, sizeof messages);
    messages.count = 3U;
    messages.entries[0].packet_id = 41U;
    messages.entries[0].peer = 0x2000U;
    messages.entries[0].broadcast = true;
    snprintf(messages.entries[0].text, sizeof messages.entries[0].text, "%s", "net check");
    messages.entries[1].packet_id = 42U;
    messages.entries[1].peer = 0x3000U;
    messages.entries[1].direction = MESH_MESSAGE_INBOUND;
    snprintf(messages.entries[1].text, sizeof messages.entries[1].text, "%s", "are you there");
    messages.entries[2].packet_id = 43U;
    messages.entries[2].peer = 0x3000U;
    messages.entries[2].direction = MESH_MESSAGE_OUTBOUND;
    snprintf(messages.entries[2].text, sizeof messages.entries[2].text, "%s", "on my way");
    mesh_ui_store_set_messages(&app.ui_store, &messages);
    app.ui_messages_cached = messages;

    /* Down to the direct conversation: All traffic, #Primary, then the peer. */
    mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_DOWN);
    mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_DOWN);
    struct mesh_ui_conversation conversation;
    if (!mesh_ui_nav_conversation_at(
            &app.ui_store, app.ui_store.nav.cursor[MESH_UI_SCREEN_MESSAGES], &conversation) ||
        conversation.kind != MESH_UI_CONVERSATION_DIRECT || conversation.node != 0x3000U) {
        failure = "expected the cursor on the direct conversation";
        goto cleanup;
    }

    /* One X only arms it: a press that lands here by accident costs nothing. */
    mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_X);
    if (!app.ui_store.nav.messages_delete_armed || app.ui_store.messages.count != 3U) {
        failure = "the first X should arm the delete without touching the log";
        goto cleanup;
    }

    mesh_ui_controller_handle_key(&app.ui_controller, MESH_UI_KEY_X);
    if (app.ui_store.messages.count != 1U || !app.ui_store.messages.entries[0].broadcast) {
        failure = "the second X should leave only the broadcast in the store";
        goto cleanup;
    }
    if (app.ui_messages_cached.count != 1U) {
        failure = "the history read back from the cache still holds the deleted conversation";
        goto cleanup;
    }
    if (app.ui_store.nav.toast[0] == '\0') {
        failure = "the delete should say what it did";
        goto cleanup;
    }

    /* A publish is what would put them back, so run one and check that it does not. */
    mesh_app_publish_ui_state(&app);
    if (app.ui_store.messages.count != 1U) {
        failure = "the next publish brought the deleted conversation back";
        goto cleanup;
    }

    /* And the cache on disk agrees, so a restart does not resurrect it either. */
    struct mesh_ui_store reloaded;
    if (mesh_ui_store_init(&reloaded) != 0) {
        failure = "reload store init failed";
        goto cleanup;
    }
    const int loaded = mesh_ui_store_load(&reloaded, app.ui_handshake_cache_path);
    const uint32_t reloaded_count = reloaded.messages.count;
    uint32_t survivors = 0U;
    for (uint32_t i = 0; i < reloaded_count && i < MESH_UI_MAX_MESSAGES; ++i) {
        if (!reloaded.messages.entries[i].broadcast &&
            reloaded.messages.entries[i].peer == 0x3000U) {
            survivors++;
        }
    }
    mesh_ui_store_shutdown(&reloaded);
    if (loaded != 0) {
        failure = "the cache should have been written back after the delete";
        goto cleanup;
    }
    if (survivors != 0U) {
        failure = "the deleted conversation is still in the cache on disk";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    unsetenv("MESHCLIENT_UI_BACKEND");
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(app_publish_cache_invalidates_data_dependencies, unit) {
    struct mesh_app *app = calloc(1U, sizeof *app);
    MESH_TEST_FAIL_IF(app == NULL, "app allocation failed");
    mesh_session_init(&app->session);
    if (mesh_ui_store_init(&app->ui_store) != 0) {
        free(app);
        record_failure(test_name, "store init failed");
        return;
    }
    struct mesh_bluez_mock_config mock = {0};
    mesh_bluez_client_mock_enable(&mock);
    const char *failure = NULL;
    struct mesh_handshake_status *handshake = &app->session.handshake;
    handshake->has_my_info = true;
    handshake->my_info.my_node_num = 1U;
    handshake->config_complete = true;
    handshake->node_count = 2U;
    handshake->nodes[0].node_id = 1U;
    handshake->nodes[1].node_id = 2U;
    snprintf(handshake->nodes[1].short_name, sizeof handshake->nodes[1].short_name, "OLD");
    struct mesh_message message = {0};
    message.packet_id = 100U;
    message.from = 2U;
    message.to = 1U;
    message.direction = MESH_MESSAGE_INBOUND;
    snprintf(message.text, sizeof message.text, "hello");
    mesh_message_log_append(&app->session.messages, &message);
    mesh_app_publish_ui_state(app);
    mesh_app_publish_ui_state(app); /* warm unchanged inputs */
    snprintf(handshake->nodes[1].short_name, sizeof handshake->nodes[1].short_name, "NEW");
    mesh_app_publish_ui_state(app);
    if (strcmp(app->ui_store.messages.entries[0].peer_name, "NEW") != 0) {
        failure = "renaming a node must invalidate formatted message names";
        goto cleanup;
    }
    struct mesh_message *live = mesh_message_log_find(&app->session.messages, 100U);
    snprintf(live->text, sizeof live->text, "edited");
    mesh_app_publish_ui_state(app);
    if (strcmp(app->ui_store.messages.entries[0].text, "edited") != 0) {
        failure = "in-place message changes must invalidate the cached view";
        goto cleanup;
    }
    handshake->nodes[1].is_favorite = true;
    mesh_app_publish_ui_state(app);
    if (!app->ui_store.handshake.nodes[1].is_favorite) {
        failure = "favorite changes must reach the cached roster";
        goto cleanup;
    }
    app->session.roster_node = 2U;
    mesh_app_publish_ui_state(app);
    if (app->ui_store.handshake.roster_owner != 2U) {
        failure = "roster ownership must be an independent invalidation input";
        goto cleanup;
    }
    /* Disconnect resets the live handshake/settings while retaining messages. */
    mesh_session_detach(&app->session);
    mesh_app_publish_ui_state(app);
    if (app->ui_store.handshake.config_complete || app->ui_store.messages.count != 1U) {
        failure = "disconnect must clear live state without losing cached messages";
    }
cleanup:
    free(app->publish_cache);
    mesh_ui_store_shutdown(&app->ui_store);
    free(app);
    mesh_bluez_client_mock_disable();
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * What the client knows against what it is showing.
 *
 * The session roster holds 256 nodes and the UI publishes its best 128, so on a busy mesh half
 * of what we know silently is not on the list. `nodes_known` is the number that makes the gap
 * sayable; without it the Nodes tab could only compare itself against the *radio's* database,
 * which is a different set and, after a NodeDB reset, the smaller one.
 */
MESH_TEST_CASE(app_publish_reports_known_against_shown, unit) {
    struct mesh_app *app = calloc(1U, sizeof *app);
    MESH_TEST_FAIL_IF(app == NULL, "app allocation failed");
    mesh_session_init(&app->session);
    if (mesh_ui_store_init(&app->ui_store) != 0) {
        free(app);
        record_failure(test_name, "store init failed");
        return;
    }
    struct mesh_bluez_mock_config mock = {0};
    mesh_bluez_client_mock_enable(&mock);
    const char *failure = NULL;

    struct mesh_handshake_status *handshake = &app->session.handshake;
    handshake->has_my_info = true;
    handshake->my_info.my_node_num = 1U;
    handshake->config_complete = true;

    /* A roster comfortably past what the UI can carry, and all of it ours: the radio's own
       count is left at 0 so the only number that can explain the gap is the roster's. */
    const uint32_t known = 200U;
    handshake->node_count = known;
    for (uint32_t i = 0; i < known; ++i) {
        handshake->nodes[i].node_id = i + 1U;
        handshake->nodes[i].last_heard = 1750000000U - i;
        handshake->nodes[i].in_nodedb = true;
    }
    mesh_app_publish_ui_state(app);

    if (app->ui_store.handshake.node_count != MESH_UI_MAX_HANDSHAKE_NODES) {
        failure = "the publish should fill the UI's roster budget";
        goto cleanup;
    }
    if (app->ui_store.handshake.nodes_known != known) {
        failure = "the roster's own total should be published beside what was shown";
        goto cleanup;
    }

    /* A roster that fits needs no gap, and must not invent one: the two numbers agree, which
       is what keeps the title from reading "12 of 12". */
    handshake->node_count = 12U;
    mesh_app_publish_ui_state(app);
    if (app->ui_store.handshake.node_count != 12U || app->ui_store.handshake.nodes_known != 12U) {
        failure = "a roster inside the budget should report one number, not two";
    }

cleanup:
    free(app->publish_cache);
    mesh_ui_store_shutdown(&app->ui_store);
    free(app);
    mesh_bluez_client_mock_disable();
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(app_init_from_dirty_storage, unit) {
    char temp_dir[] = "/tmp/mesh_app_dirty_initXXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(temp_dir) == NULL, "temporary directory creation failed");
    const char *original_home = getenv("HOME");
    const char *original_backend = getenv("MESHCLIENT_UI_BACKEND");
    char *saved_home = original_home != NULL ? strdup(original_home) : NULL;
    char *saved_backend = original_backend != NULL ? strdup(original_backend) : NULL;
    setenv("HOME", temp_dir, 1);
    setenv("MESHCLIENT_UI_BACKEND", "stub", 1);
    struct mesh_bluez_mock_config mock = {0};
    mesh_bluez_client_mock_enable(&mock);
    const char *failure = NULL;
    for (unsigned publish = 0U; publish < 2U; ++publish) {
        struct mesh_app app;
        memset(&app, 0xA5, sizeof app);
        app.config = mesh_app_config_default();
        app.config.enable_ble = false;
        app.config.enable_serial = false;
        app.config.idle_timeout_ms = 17;
        if (mesh_app_init(&app, &app.config) != 0) {
            failure = "initialization from nonzero storage failed";
            break;
        }
        /* Keep the regression test entirely in memory, including shutdown. */
        app.ui_preferences_path[0] = '\0';
        app.ui_handshake_cache_path[0] = '\0';
        if (app.publish_cache != NULL) {
            failure = "initialization must clear the lazy publication cache";
            app.publish_cache = NULL; /* report cleanly instead of freeing the poison value */
        } else if (app.config.enable_ble || app.config.enable_serial ||
                   app.config.idle_timeout_ms != 17) {
            failure = "initialization must preserve an aliased config";
        } else if (publish != 0U) {
            mesh_app_publish_ui_state(&app);
            if (app.publish_cache == NULL) {
                failure = "first publication must allocate the cache";
            }
        }
        mesh_app_shutdown(&app);
        if (app.publish_cache != NULL) {
            failure = "shutdown must release and clear the cache";
        }
        if (failure != NULL) {
            break;
        }
    }
    mesh_bluez_client_mock_disable();
    if (saved_home != NULL) {
        setenv("HOME", saved_home, 1);
    } else {
        unsetenv("HOME");
    }
    if (saved_backend != NULL) {
        setenv("MESHCLIENT_UI_BACKEND", saved_backend, 1);
    } else {
        unsetenv("MESHCLIENT_UI_BACKEND");
    }
    free(saved_home);
    free(saved_backend);
    char prefs_dir[256];
    snprintf(prefs_dir, sizeof prefs_dir, "%s/.meshclient", temp_dir);
    rmdir(prefs_dir);
    rmdir(temp_dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(app_cache_batches_and_retries_persistence, unit) {
    struct mesh_app *app = calloc(1U, sizeof *app);
    MESH_TEST_FAIL_IF(app == NULL, "allocation failed");
    const char *failure = NULL;
    char path[] = "/tmp/mesh-cache-batch-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || mesh_event_loop_init(&app->loop) != 0) {
        if (fd >= 0)
            close(fd);
        unlink(path);
        free(app);
        record_failure(test_name, "fixture init failed");
        return;
    }
    close(fd);
    unlink(path);
    mesh_session_init(&app->session);
    mesh_ui_store_init(&app->ui_store);
    snprintf(app->ui_handshake_cache_path, sizeof app->ui_handshake_cache_path, "%s", path);
    app->ui_handshake_cache_dirty = true;
    mesh_app_publish_ui_state(app);
    const int timer = app->ui_cache_timer_fd;
    struct itimerspec before, after;
    timerfd_gettime(timer, &before);
    app->ui_store.read_state.revision++;
    mesh_app_publish_ui_state(app);
    timerfd_gettime(timer, &after);
    if (!app->ui_cache_timer_armed || timer != app->ui_cache_timer_fd || access(path, F_OK) == 0 ||
        after.it_value.tv_sec > before.it_value.tv_sec ||
        (after.it_value.tv_sec == before.it_value.tv_sec &&
         after.it_value.tv_nsec > before.it_value.tv_nsec)) {
        failure = "updates must share a fixed batching deadline without writing immediately";
        goto cleanup;
    }
    const struct itimerspec expire = {.it_value = {.tv_nsec = 1L}};
    timerfd_settime(timer, 0, &expire, NULL);
    struct pollfd ready_fd = {.fd = timer, .events = POLLIN};
    (void)poll(&ready_fd, 1, 1000);
    mesh_event_loop_run(&app->loop, 0);
    if (app->ui_handshake_cache_dirty || app->ui_cache_timer_armed || access(path, F_OK) != 0) {
        failure = "timer must persist and clear the dirty batch";
        goto cleanup;
    }
    /*
     * A path that cannot be written, to check that a failed save keeps the batch rather than
     * dropping it. It names a file *under* /dev/full rather than /dev/full itself: the cache is
     * written through a `.tmp` beside it and renamed, so a device that accepts an open and
     * refuses the bytes no longer stands in for a card that cannot take the file at all. Every
     * open under a non-directory fails with ENOTDIR, whoever is running the suite.
     */
    snprintf(app->ui_handshake_cache_path, sizeof app->ui_handshake_cache_path, "/dev/full/cache");
    app->ui_handshake_cache_dirty = true;
    mesh_app_publish_ui_state(app);
    timerfd_settime(app->ui_cache_timer_fd, 0, &expire, NULL);
    ready_fd.fd = app->ui_cache_timer_fd;
    (void)poll(&ready_fd, 1, 1000);
    mesh_event_loop_run(&app->loop, 0);
    if (!app->ui_handshake_cache_dirty || !app->ui_cache_timer_armed) {
        failure = "a save that could not be written must stay dirty and schedule another batch";
        goto cleanup;
    }
    snprintf(app->ui_handshake_cache_path, sizeof app->ui_handshake_cache_path, "%s", path);
    mesh_app_flush_ui_cache(app);
    if (app->ui_handshake_cache_dirty)
        failure = "explicit final flush must persist a pending batch";
cleanup:
    mesh_app_close_ui_cache_timer(app);
    mesh_ui_store_shutdown(&app->ui_store);
    mesh_event_loop_shutdown(&app->loop);
    free(app->publish_cache);
    free(app);
    unlink(path);
    if (failure != NULL)
        record_failure(test_name, failure);
    else
        record_success(test_name);
}

/*
 * The two sections whose write is not a Config, a ModuleConfig or a Channel.
 *
 * Both are here for the same reason, which is the one thing a save must never do: lose
 * something the screen could not show. The UI config carries a touchscreen calibration and a
 * map home point that this client has no rows for, so the write has to be the radio's own
 * record with the edits laid over it rather than a fresh one built from the rows. The canned
 * list is the same problem in a different shape - a radio holding more messages than the
 * section has slots keeps them, and an emptied slot closes up rather than leaving a blank
 * quick reply behind it, the way a cleared admin key does.
 */
MESH_TEST_CASE(app_extra_section_writes, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);

    radio.has_ui_config = true;
    radio.ui_config.screen_brightness = 40U;
    radio.ui_config.theme = meshtastic_Theme_DARK;
    radio.ui_config.screen_lock = true;
    radio.ui_config.calibration_data.size = 4U;
    radio.ui_config.calibration_data.bytes[0] = 0xAB;
    radio.ui_config.has_map_data = true;
    radio.ui_config.map_data.follow_gps = true;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_RADIO_UI;
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_UI_BRIGHTNESS;
    action.edits[0].number = 200U;
    action.edits[1].field = MESH_UI_FIELD_UI_THEME;
    action.edits[1].number = 2U; /* RED */

    struct mesh_admin_request write;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.kind != MESH_ADMIN_SET_UI_CONFIG ||
                          write.payload.ui_config.screen_brightness != 200U ||
                          write.payload.ui_config.theme != meshtastic_Theme_RED,
                      "a Radio UI save should carry the edits");
    MESH_TEST_FAIL_IF(write.payload.ui_config.calibration_data.size != 4U ||
                          write.payload.ui_config.calibration_data.bytes[0] != 0xAB ||
                          !write.payload.ui_config.has_map_data ||
                          !write.payload.ui_config.map_data.follow_gps ||
                          !write.payload.ui_config.screen_lock,
                      "a Radio UI save must not erase what the section has no rows for");

    /* Eight messages on the radio, six slots on the screen: edit the first, empty the third,
       and the two past the last slot have to come back untouched. */
    radio.has_canned_messages = true;
    snprintf(radio.canned_messages, sizeof radio.canned_messages, "%s",
             "one|two|three|four|five|six|seven|eight");
    action.section = MESH_UI_SETTINGS_CANNED;
    action.edit_count = 2U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_CANNED_0;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "On my way");
    action.edits[1].field = MESH_UI_FIELD_CANNED_2; /* cleared */
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.kind != MESH_ADMIN_SET_CANNED_MESSAGES ||
                          strcmp(write.payload.text, "On my way|two|four|five|six|seven|eight") !=
                              0,
                      "a canned save should compact the gaps and keep what it could not show");

    /*
     * The overflow the slot count cannot prevent. Six rows of 32 always fit the wire's 200,
     * but a radio holding more than six can have a tail long enough that lengthening a visible
     * message pushes it over - and a join that stopped at the cap would send the list without
     * that tail, which the radio reads as a deletion. So the save is refused instead.
     */
    snprintf(radio.canned_messages, sizeof radio.canned_messages, "a|b|c|d|e|f|%s",
             "0123456789012345678901234567890123456789012345678901234567890123456789"
             "0123456789012345678901234567890123456789012345678901234567890123456789"
             "0123456789012345678901");
    action.edit_count = 1U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_CANNED_0;
    /* 31 characters: the per-slot cap, which is the longest a row can legitimately become. */
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s",
             "a much longer first message xyz");
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -E2BIG,
                      "a canned save that would drop the hidden tail must be refused");
    /* The same radio without the lengthening edit still fits, so the refusal is about the
       edit rather than about the radio being unwritable. */
    action.edit_count = 0U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0,
                      "the list the radio already holds must still be writable");

    mesh_radio_settings_reset(&radio);
    action.section = MESH_UI_SETTINGS_RADIO_UI;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOENT,
                      "a UI config the radio has not sent cannot be written");
    action.section = MESH_UI_SETTINGS_CANNED;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -ENOENT,
                      "a canned list the radio has not sent cannot be written");
    record_success(test_name);
}

/*
 * The map's roster is published from the whole session roster, not from the ranked rows.
 *
 * The unit tests next door hand mesh_ui_map_build() a roster built by hand; this is the other
 * half - that the ranking cut actually produces one. It is the seam the change is about: the
 * session holds MESH_SESSION_MAX_NODES, the rows carry MESH_UI_MAX_HANDSHAKE_NODES of them, and
 * before this the map was drawing the rows. A node's rank says how likely you are to talk to
 * it, which has nothing to do with whether its marker belongs on the panel.
 *
 * Seeded with more positioned nodes than there are rows, which is what makes the two counts
 * differ - on a mesh where every node had a fix, the map's roster is exactly twice the list's.
 */
MESH_TEST_CASE(app_publishes_the_map_roster_past_the_ranking_cut, unit) {
    struct mesh_app app;
    bool app_ready = false;
    const char *failure = NULL;

    setenv("MESHCLIENT_UI_BACKEND", "stub", 1);
    unsetenv("MESHCLIENT_AUTOCONNECT");

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_SINGLE_POLL;
    config.enable_serial = false;
    config.enable_ble = false;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    /* Every node positioned, and every one heard longer ago than the last - so the ranking's
       order is the seeding order and "beyond the cut" means "seeded late". */
    const uint32_t seeded = MESH_UI_MAX_HANDSHAKE_NODES + 40U;
    for (uint32_t i = 0; i < seeded; ++i) {
        struct mesh_node_summary node;
        memset(&node, 0, sizeof node);
        node.node_id = 0x50000000U + i;
        node.in_nodedb = true;
        node.last_heard = 1000000U - i;
        snprintf(node.short_name, sizeof node.short_name, "N%03u", i % 1000U);
        snprintf(node.long_name, sizeof node.long_name, "Node %u", i);
        node.position.valid = true;
        node.position.latitude_i = 476180000 + (int32_t)i * 3000;
        node.position.longitude_i = -1223320000 + (int32_t)i * 3000;
        node.position.precision_bits = 16U;
        mesh_session_seed_node(&app.session, &node);
    }

    mesh_app_publish_ui_state(&app);
    const struct mesh_ui_handshake_state *hs = &app.ui_store.handshake;

    if (hs->node_count != MESH_UI_MAX_HANDSHAKE_NODES) {
        failure = "the list should still publish exactly its own budget of rows";
        goto cleanup;
    }
    if (hs->nodes_known != seeded) {
        failure = "and should still report the roster's own total beside it";
        goto cleanup;
    }
    if (hs->map_node_count != seeded) {
        failure = "while the map's roster carries every positioned node the session holds";
        goto cleanup;
    }

    /* The last one seeded: past the cut, so it has a marker and no row. */
    const struct mesh_ui_map_node *last = &hs->map_nodes[seeded - 1U];
    if (last->node_id != 0x50000000U + seeded - 1U) {
        failure = "the map's roster should be in the same ranked order as the rows";
        goto cleanup;
    }
    if (last->has_row) {
        failure = "a node past the cut should say it has no row";
        goto cleanup;
    }
    if (mesh_ui_node_detail_find(hs, last->node_id) != NULL) {
        failure = "and should genuinely have none, or the flag is describing nothing";
        goto cleanup;
    }
    if (strcmp(last->label, "N167") != 0) {
        failure = "carrying the short name the map draws beside it";
        goto cleanup;
    }
    if (last->precision_bits != 16U) {
        failure = "and the rounding its sender declared";
        goto cleanup;
    }

    /* And one inside the cut, to show has_row is the cut and not a constant. */
    if (!hs->map_nodes[0].has_row ||
        mesh_ui_node_detail_find(hs, hs->map_nodes[0].node_id) == NULL) {
        failure = "a node the list published should say so, and be findable by id";
        goto cleanup;
    }

    /* What the map makes of it: a marker each, and a badge counting the roster. */
    struct mesh_ui_map_view view;
    mesh_ui_map_build(&app.ui_store, &view);
    if (view.count != seeded || view.known != seeded) {
        failure = "every positioned node should reach the map as a marker";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    unsetenv("MESHCLIENT_UI_BACKEND");
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A direct message announcing itself, and the two ways that must not misfire.
 *
 * The reporter is driven through mesh_app_publish_ui_state() rather than called directly,
 * because when it runs relative to the rest of a publish is half of what it promises.
 */
MESH_TEST_CASE(app_direct_message_notice, unit) {
    struct mesh_app *app = calloc(1U, sizeof *app);
    if (app == NULL) {
        record_failure(test_name, "out of memory");
        return;
    }
    if (mesh_ui_store_init(&app->ui_store) != 0) {
        free(app);
        record_failure(test_name, "store init failed");
        return;
    }
    struct mesh_bluez_mock_config mock = {0};
    mesh_bluez_client_mock_enable(&mock);
    const char *failure = NULL;

    app->config.run_mode = MESH_APP_RUN_FOREGROUND;
    struct mesh_handshake_status *handshake = &app->session.handshake;
    handshake->has_my_info = true;
    handshake->my_info.my_node_num = 1U;
    handshake->config_complete = true;
    handshake->node_count = 3U;
    handshake->nodes[0].node_id = 1U;
    handshake->nodes[1].node_id = 2U;
    snprintf(handshake->nodes[1].short_name, sizeof handshake->nodes[1].short_name, "ALFA");
    handshake->nodes[2].node_id = 3U;
    snprintf(handshake->nodes[2].short_name, sizeof handshake->nodes[2].short_name, "BRVO");

    struct mesh_message message = {0};
    message.from = 2U;
    message.to = 1U;
    message.direction = MESH_MESSAGE_INBOUND;

    /*
     * The cache's worth of history, adopted in silence. The log is seeded before the first
     * publish, so anything already in it may be something the user was shown days ago.
     */
    message.packet_id = 100U;
    snprintf(message.text, sizeof message.text, "%s", "from before the launch");
    mesh_message_log_append(&app->session.messages, &message);
    mesh_app_publish_ui_state(app);
    if (app->ui_store.nav.toast[0] != '\0') {
        failure = "a launch should not announce what the cache brought with it";
        goto cleanup;
    }

    /* Now one that genuinely arrives. */
    message.packet_id = 101U;
    snprintf(message.text, sizeof message.text, "%s", "are you heading out");
    mesh_message_log_append(&app->session.messages, &message);
    mesh_app_publish_ui_state(app);
    if (strstr(app->ui_store.nav.toast, "ALFA") == NULL ||
        strstr(app->ui_store.nav.toast, "heading out") == NULL) {
        failure = "a direct message should say who sent it and what they said";
        goto cleanup;
    }

    /* A broadcast is not news: a channel is a room full of people talking. */
    mesh_ui_store_set_toast(&app->ui_store, test_now_ms(), "");
    message.packet_id = 102U;
    message.to = MESH_MESSAGE_BROADCAST_ADDR;
    snprintf(message.text, sizeof message.text, "%s", "anyone on the ridge");
    mesh_message_log_append(&app->session.messages, &message);
    mesh_app_publish_ui_state(app);
    if (app->ui_store.nav.toast[0] != '\0') {
        failure = "a broadcast should raise nothing";
        goto cleanup;
    }

    /*
     * And the one the review caught: deleting the conversation the cursor was pointing into.
     *
     * The announced packet is no longer in the log, which is indistinguishable from the ring
     * having evicted it - so a reporter that treated "everything since" as new would announce
     * whatever inbound message happened to be last. Here that is BRVO's older one, which the
     * user has already been told about, arriving as a notice a second after they pressed delete.
     */
    message.packet_id = 50U;
    message.from = 3U;
    message.to = 1U;
    snprintf(message.text, sizeof message.text, "%s", "older, and already announced");
    mesh_message_log_append(&app->session.messages, &message);
    mesh_app_publish_ui_state(app);
    mesh_ui_store_set_toast(&app->ui_store, test_now_ms(), "");
    /* Delete ALFA's conversation, which is where packet 101 - the cursor - lives. */
    (void)mesh_message_log_forget(&app->session.messages, 2U, 0U);
    mesh_app_publish_ui_state(app);
    if (app->ui_store.nav.toast[0] != '\0') {
        failure = "losing the cursor should take our place again silently, not re-announce";
        goto cleanup;
    }
    /* And the client has taken its place again, so the next real arrival still speaks. */
    message.packet_id = 103U;
    message.from = 3U;
    snprintf(message.text, sizeof message.text, "%s", "genuinely new");
    mesh_message_log_append(&app->session.messages, &message);
    mesh_app_publish_ui_state(app);
    if (strstr(app->ui_store.nav.toast, "genuinely new") == NULL) {
        failure = "the reporter should still be live after relocating its cursor";
        goto cleanup;
    }

    /*
     * And the third way it must not misfire: a Store & Forward window.
     *
     * A replayed direct message is inbound, addressed to us, text, and now carries a real packet
     * id from StoreAndForward.original_id - so every structural gate above it passes. It is still
     * not news: radio_request_history() promises the fetched messages arrive in the transcript
     * and are counted by the section's own row, and a window of them would otherwise come back as
     * a burst of notices about conversations from hours ago.
     */
    mesh_ui_store_set_toast(&app->ui_store, test_now_ms(), "");
    message.packet_id = 0x9001U;
    message.from = 2U;
    message.replayed = true;
    snprintf(message.text, sizeof message.text, "%s", "said four hours ago");
    mesh_message_log_append(&app->session.messages, &message);
    mesh_app_publish_ui_state(app);
    if (app->ui_store.nav.toast[0] != '\0') {
        failure = "a replayed message is history the user asked for, not a notice";
        goto cleanup;
    }

    /* And the window has not wedged the cursor: what arrives next still speaks. */
    message.packet_id = 104U;
    message.replayed = false;
    snprintf(message.text, sizeof message.text, "%s", "live again");
    mesh_message_log_append(&app->session.messages, &message);
    mesh_app_publish_ui_state(app);
    if (strstr(app->ui_store.nav.toast, "live again") == NULL) {
        failure = "a replayed window should not stop the next real arrival being announced";
        goto cleanup;
    }

cleanup:
    /* The publish cache is lazily allocated by the first publish, exactly as it is on a device,
       and this app was never through mesh_app_shutdown() to have it released. */
    free(app->publish_cache);
    mesh_ui_store_shutdown(&app->ui_store);
    mesh_bluez_client_mock_disable();
    free(app);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Unmuting a conversation the radio is also muting says so rather than claiming success.
 *
 * Both halves can be on at once - mute here, then mute the same node from the Nodes tab or from
 * another client - and the local half really does clear. What does not change is the row, which
 * the radio goes on muting; "Unmuted" there is the press lying about what it did.
 */
MESH_TEST_CASE(app_unmute_reports_the_radios_mute, unit) {
    struct mesh_app *app = calloc(1U, sizeof *app);
    if (app == NULL) {
        record_failure(test_name, "out of memory");
        return;
    }
    if (mesh_ui_store_init(&app->ui_store) != 0) {
        free(app);
        record_failure(test_name, "store init failed");
        return;
    }
    struct mesh_bluez_mock_config mock = {0};
    mesh_bluez_client_mock_enable(&mock);
    const char *failure = NULL;

    app->config.run_mode = MESH_APP_RUN_FOREGROUND;
    struct mesh_ui_handshake_state published;
    memset(&published, 0, sizeof published);
    published.node_count = 1U;
    published.nodes[0].node_id = 0x2000U;
    snprintf(published.nodes[0].short_name, sizeof published.nodes[0].short_name, "ALFA");
    mesh_ui_store_set_handshake(&app->ui_store, &published);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_MUTE_CONVERSATION;
    action.number = (uint32_t)MESH_UI_CONVERSATION_DIRECT;
    action.dest = 0x2000U;
    snprintf(action.text, sizeof action.text, "%s", "ALFA");

    /* Muted here, and then muted on the radio as well. */
    mesh_app_on_ui_action(app, &action);
    if (!mesh_ui_store_conversation_muted_locally(
            &app->ui_store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x2000U, 0U)) {
        failure = "the press should have muted it locally";
        goto cleanup;
    }
    published.nodes[0].is_muted = true;
    mesh_ui_store_set_handshake(&app->ui_store, &published);

    /* The unmute: the local flag clears, and the notice tells the truth about the outcome. */
    mesh_app_on_ui_action(app, &action);
    if (mesh_ui_store_conversation_muted_locally(
            &app->ui_store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x2000U, 0U)) {
        failure = "the press should still have cleared this client's own mute";
        goto cleanup;
    }
    char expected[MESH_UI_NAV_TOAST_MAX];
    mesh_str_format(expected, sizeof expected, MESH_STR_TOAST_CONVO_MUTED_ON_RADIO, "ALFA");
    if (strcmp(app->ui_store.nav.toast, expected) != 0) {
        failure = "an unmute the radio overrides should name the radio, not claim success";
        goto cleanup;
    }
    /*
     * While the radio is still muting it, the press has nothing to clear and says so rather than
     * muting again - the row reads "unmute", so a press that muted would be going the wrong way.
     */
    mesh_app_on_ui_action(app, &action);
    if (mesh_ui_store_conversation_muted_locally(
            &app->ui_store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x2000U, 0U)) {
        failure = "pressing unmute on a radio-muted row must not mute it locally instead";
        goto cleanup;
    }

    /* With the radio's flag gone the row reads "mute" again, so the two presses go both ways. */
    published.nodes[0].is_muted = false;
    mesh_ui_store_set_handshake(&app->ui_store, &published);
    mesh_app_on_ui_action(app, &action); /* mute */
    mesh_app_on_ui_action(app, &action); /* and unmute */
    mesh_str_format(expected, sizeof expected, MESH_STR_TOAST_CONVO_UNMUTED, "ALFA");
    if (strcmp(app->ui_store.nav.toast, expected) != 0) {
        failure = "an unmute with nothing else muting it should say so plainly";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&app->ui_store);
    mesh_bluez_client_mock_disable();
    free(app);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * What the radio says about its network and about its own build, on the way to the two screens
 * that read them.
 *
 * Both are a straight copy through mesh_app_flatten_settings(), and both were bytes that
 * arrived and stopped there until the sections that read them existed: NETWORK_CONFIG has been
 * fetched on every refresh since phase 1, and excluded_modules has been in DeviceMetadata since
 * before this client read metadata at all. A copy nobody asserts is a copy that can be dropped
 * in a merge without a single test going red, which is how they came to be missing in the first
 * place.
 *
 * wifi_psk is the one field here asserted by its *absence*: the store has no member for it, so
 * the credential stops at the radio record. That is checked by the compiler rather than by a
 * line below, and said here because it is a decision rather than an omission.
 */
MESH_TEST_CASE(app_publishes_network_and_build_facts, unit) {
    const char *failure = NULL;
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "network")) {
        record_failure(test_name, "mkdtemp failed");
        return;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_serial = false;
    config.enable_ble = false;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    meshtastic_Config network = meshtastic_Config_init_zero;
    network.which_payload_variant = meshtastic_Config_network_tag;
    network.payload_variant.network.wifi_enabled = true;
    snprintf(network.payload_variant.network.wifi_ssid,
             sizeof network.payload_variant.network.wifi_ssid, "%s", "Shed");
    snprintf(network.payload_variant.network.wifi_psk,
             sizeof network.payload_variant.network.wifi_psk, "%s", "hunter2");
    network.payload_variant.network.address_mode =
        meshtastic_Config_NetworkConfig_AddressMode_STATIC;
    network.payload_variant.network.has_ipv4_config = true;
    network.payload_variant.network.ipv4_config.gateway = 0x0101A8C0U; /* 192.168.1.1 */
    snprintf(network.payload_variant.network.ntp_server,
             sizeof network.payload_variant.network.ntp_server, "%s", "ntp.example.invalid");
    network.payload_variant.network.enabled_protocols =
        meshtastic_Config_NetworkConfig_ProtocolFlags_UDP_BROADCAST;
    mesh_radio_settings_apply_config(&app.session.settings, &network);

    app.session.settings.has_metadata = true;
    app.session.settings.metadata.excluded_modules =
        meshtastic_ExcludedModules_MQTT_CONFIG | meshtastic_ExcludedModules_NETWORK_CONFIG;
    app.session.settings.metadata.has_xeddsa = true;

    mesh_app_publish_ui_state(&app);

    const struct mesh_ui_settings *ui = &app.ui_store.settings;
    if (!ui->has_network || !ui->wifi_enabled || strcmp(ui->wifi_ssid, "Shed") != 0 ||
        ui->address_mode != (uint8_t)meshtastic_Config_NetworkConfig_AddressMode_STATIC ||
        ui->ipv4_gateway != 0x0101A8C0U || strcmp(ui->ntp_server, "ntp.example.invalid") != 0 ||
        ui->enabled_protocols != meshtastic_Config_NetworkConfig_ProtocolFlags_UDP_BROADCAST) {
        failure = "the network section the radio sent should reach the store whole";
        goto cleanup;
    }
    if (ui->excluded_modules !=
            (meshtastic_ExcludedModules_MQTT_CONFIG | meshtastic_ExcludedModules_NETWORK_CONFIG) ||
        !ui->has_xeddsa) {
        failure = "what the firmware was built without should reach the store too";
        goto cleanup;
    }
    /* And the two lists that read it agree about the same radio: a section whose bit is set is
       excluded rather than late, whichever list the row is in. */
    if (mesh_ui_settings_section_availability(ui, &app.ui_store.handshake, MESH_UI_SETTINGS_MQTT) !=
        MESH_UI_SETTINGS_SECTION_EXCLUDED) {
        failure = "an excluded module should read as excluded once published";
        goto cleanup;
    }
    /* Network is the exception in that mask: its bit is set, and the radio sent the section
       anyway, so there are rows and the row that says why there are none must not appear. */
    if (mesh_ui_settings_section_availability(ui, &app.ui_store.handshake,
                                              MESH_UI_SETTINGS_NETWORK) !=
        MESH_UI_SETTINGS_SECTION_READY) {
        failure = "a section the radio sent is ready whatever its bit says";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A save must carry back the fields this client has no row for.
 *
 * The firmware *replaces* a section on set_config - it assigns rather than merging - so the only
 * correct base for a write is the radio's own record with the pending edits applied on top.
 * mesh_app_build_settings_write does that, and this pins it: a write builder that ever started from
 * a fresh struct would silently flatten a radio's frequency override, its position flags and its
 * deep-sleep timer the first time somebody changed a hop limit.
 *
 * Every field asserted here is one the tab deliberately does not offer, which is exactly why
 * nothing else in the suite would notice it going missing.
 */
/*
 * One flag row pressed sets one bit, and leaves the other nine where the radio had them.
 *
 * The failure this exists for is the one a bitfield invites: a write that *assigns* the word
 * instead of masking into it turns ten settings into one, and it does so silently - the row the
 * user pressed reads back correctly and the nine they did not are quietly cleared. So both
 * directions are checked, on a word that starts with some bits set and some clear.
 */
/*
 * LoRa's advanced rows on the way out: three numbers typed as text, two toggles, and the
 * repeated ignore list compacted the way a cleared admin key is.
 *
 * The compaction is the half worth a test. A repeated field with a hole in the middle is a
 * list the firmware reads as shorter than it is, so emptying the *first* of three slots has to
 * leave the other two in place rather than truncating the field at the gap.
 */
MESH_TEST_CASE(app_lora_advanced_write_build, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    radio.has_lora = true;
    radio.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    radio.lora.hop_limit = 3U;
    radio.lora.ignore_incoming_count = 3U;
    radio.lora.ignore_incoming[0] = 0x11111111U;
    radio.lora.ignore_incoming[1] = 0x22222222U;
    radio.lora.ignore_incoming[2] = 0x33333333U;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_LORA;
    action.edit_count = 5U;
    action.edits[0].field = MESH_UI_FIELD_LORA_BOOST_GAIN;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_LORA_CHANNEL_NUM;
    snprintf(action.edits[1].text, sizeof action.edits[1].text, "%s", "42");
    action.edits[2].field = MESH_UI_FIELD_LORA_OVERRIDE_FREQ;
    snprintf(action.edits[2].text, sizeof action.edits[2].text, "%s", "906.875");
    action.edits[3].field = MESH_UI_FIELD_LORA_FREQUENCY_TRIM;
    snprintf(action.edits[3].text, sizeof action.edits[3].text, "%s", "-12.5");
    /* The first slot emptied: the two behind it move up rather than being cut off. */
    action.edits[4].field = MESH_UI_FIELD_LORA_IGNORE_NODE_0;
    action.edits[4].text[0] = '\0';

    struct mesh_admin_request write;
    const meshtastic_Config_LoRaConfig *lora = &write.payload.config.payload_variant.lora;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          !lora->sx126x_rx_boosted_gain || lora->channel_num != 42U ||
                          lora->override_frequency != 906.875f ||
                          lora->frequency_offset != -12.5f || lora->ignore_incoming_count != 2U ||
                          lora->ignore_incoming[0] != 0x22222222U ||
                          lora->ignore_incoming[1] != 0x33333333U || lora->ignore_incoming[2] != 0U,
                      "the advanced rows should write, and an emptied ignore slot should close up");

    /* A typed value the radio would not take is refused here rather than sent: the toast the
       caller draws for -EINVAL is the only thing that would say a frequency was nonsense. */
    memset(&action.edits[0], 0, sizeof action.edits[0]);
    action.edit_count = 1U;
    action.edits[0].field = MESH_UI_FIELD_LORA_OVERRIDE_FREQ;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "906.8 MHz");
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -EINVAL,
                      "a frequency that is not a number should be refused");
    action.edits[0].field = MESH_UI_FIELD_LORA_CHANNEL_NUM;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "12.5");
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != -EINVAL,
                      "half a frequency slot is not a frequency slot");

    /*
     * And a ham row never reaches the section's write at all: it is filed under its own
     * consumer, so Y carrying one would be a press writing something it was not asked to.
     */
    action.edits[0].field = MESH_UI_FIELD_LORA_HAM_CALL_SIGN;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "KD2ABC");
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0,
                      "a ham row in the edit list belongs to another press and is skipped");
    record_success(test_name);
}

MESH_TEST_CASE(radio_settings_write_sets_one_position_flag, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    radio.has_position = true;
    /* Altitude, DOP and the fix time on. */
    const uint32_t held = meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE |
                          meshtastic_Config_PositionConfig_PositionFlags_DOP |
                          meshtastic_Config_PositionConfig_PositionFlags_TIMESTAMP;
    radio.position.position_flags = held;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_POSITION;
    action.edit_count = 2U;
    /* One bit on, one bit off, in one save. */
    action.edits[0].field = MESH_UI_FIELD_POSITION_FLAG_SPEED;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_POSITION_FLAG_DOP;
    action.edits[1].number = 0U;

    struct mesh_admin_request write;
    const meshtastic_Config_PositionConfig *pos = &write.payload.config.payload_variant.position;
    const uint32_t expect = (held | meshtastic_Config_PositionConfig_PositionFlags_SPEED) &
                            ~(uint32_t)meshtastic_Config_PositionConfig_PositionFlags_DOP;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          pos->position_flags != expect,
                      "a flag edit should set or clear its own bit and no other");
    record_success(test_name);
}

/*
 * The beacon's targets: four records of three rows, written by arithmetic rather than by twelve
 * arms, and compacted the way every other repeated field this client writes is.
 *
 * The three things worth holding are in one save, because they only go wrong together: an edit
 * to a record the radio never sent has to grow the list; a record whose three rows all say
 * "whatever the radio is running" is not a destination and must not go out as one; and the gap
 * that leaves has to close rather than be sent as an entry the firmware would read as a
 * duplicate of the default.
 */
MESH_TEST_CASE(radio_settings_write_compacts_the_beacon_targets, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    radio.has_mesh_beacon = true;
    /* The radio holds two targets; the first is about to be emptied and the third invented. */
    radio.mesh_beacon.broadcast_targets_count = 2U;
    radio.mesh_beacon.broadcast_targets[0].region = meshtastic_Config_LoRaConfig_RegionCode_US;
    radio.mesh_beacon.broadcast_targets[1].has_preset = true;
    radio.mesh_beacon.broadcast_targets[1].preset =
        meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW;
    /* A field with no row of its own, which the write must carry across untouched. */
    radio.mesh_beacon.broadcast_offer_region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_BEACON;
    action.edit_count = 3U;
    /* Target 1's region back to "as configured", which empties it. */
    action.edits[0].field = MESH_UI_FIELD_BEACON_TARGET_0_REGION;
    action.edits[0].number = 0U;
    /* Target 3, which the radio never sent: the list has to grow to reach it. */
    action.edits[1].field = MESH_UI_FIELD_BEACON_TARGET_2_CHANNEL;
    action.edits[1].number = 4U; /* stored one past itself: channel 3 */
    action.edits[2].field = MESH_UI_FIELD_BEACON_TARGET_2_REGION;
    action.edits[2].number = (uint32_t)meshtastic_Config_LoRaConfig_RegionCode_ANZ;

    struct mesh_admin_request write;
    const meshtastic_ModuleConfig_MeshBeaconConfig *beacon =
        &write.payload.module_config.payload_variant.mesh_beacon;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0,
                      "a beacon save should build");
    MESH_TEST_FAIL_IF(write.kind != MESH_ADMIN_SET_MODULE_CONFIG ||
                          write.type != meshtastic_AdminMessage_ModuleConfigType_MESHBEACON_CONFIG,
                      "the beacon must go out as its own module type");
    MESH_TEST_FAIL_IF(beacon->broadcast_targets_count != 2U,
                      "an emptied target should be dropped and the invented one kept");
    MESH_TEST_FAIL_IF(!beacon->broadcast_targets[0].has_preset ||
                          beacon->broadcast_targets[0].preset !=
                              meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW,
                      "the surviving target should move up rather than leave a gap");
    MESH_TEST_FAIL_IF(!beacon->broadcast_targets[1].has_channel_index ||
                          beacon->broadcast_targets[1].channel_index != 3U ||
                          beacon->broadcast_targets[1].region !=
                              meshtastic_Config_LoRaConfig_RegionCode_ANZ,
                      "the target the rows invented should carry both of its edits");
    MESH_TEST_FAIL_IF(beacon->broadcast_offer_region !=
                          meshtastic_Config_LoRaConfig_RegionCode_EU_868,
                      "a beacon save must keep the fields no row touched");
    record_success(test_name);
}

/*
 * The beacon's other two ends: a flag row is one bit of `flags`, and either row of the offered
 * channel brings the submessage holding it.
 *
 * The offered channel is the one place a ChannelSettings is written that is not a channel, so
 * this is also what says the extracted key path reaches it: "no encryption" on a beacon key
 * clears the same psk a channel row would.
 */
MESH_TEST_CASE(radio_settings_write_sets_a_beacon_flag_and_its_offer, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    radio.has_mesh_beacon = true;
    radio.mesh_beacon.flags = meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LISTEN_ENABLED |
                              meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LEGACY_SPLIT;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_BEACON;
    action.edit_count = 4U;
    action.edits[0].field = MESH_UI_FIELD_BEACON_BROADCAST;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_BEACON_LEGACY_SPLIT;
    action.edits[1].number = 0U;
    action.edits[2].field = MESH_UI_FIELD_BEACON_OFFER_NAME;
    snprintf(action.edits[2].text, sizeof action.edits[2].text, "Welcome");
    action.edits[3].field = MESH_UI_FIELD_BEACON_OFFER_PRESET;
    action.edits[3].number = 3U; /* one past itself: ModemPreset 2 */

    struct mesh_admin_request write;
    const meshtastic_ModuleConfig_MeshBeaconConfig *beacon =
        &write.payload.module_config.payload_variant.mesh_beacon;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0,
                      "a beacon save should build");
    MESH_TEST_FAIL_IF(
        beacon->flags !=
            (uint32_t)(meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LISTEN_ENABLED |
                       meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_BROADCAST_ENABLED),
        "a beacon flag edit should set or clear its own bit and no other");
    MESH_TEST_FAIL_IF(!beacon->has_broadcast_offer_channel ||
                          strcmp(beacon->broadcast_offer_channel.name, "Welcome") != 0,
                      "the offered name should bring the submessage that holds it");
    MESH_TEST_FAIL_IF(!beacon->has_broadcast_offer_preset ||
                          beacon->broadcast_offer_preset !=
                              (meshtastic_Config_LoRaConfig_ModemPreset)2,
                      "an offered preset is stored one past itself and written back as itself");

    /* And the key path, which the beacon shares with the Channels section. The name goes with
       it because the name is what keeps the submessage - see
       radio_settings_write_drops_a_nameless_beacon_offer. */
    memset(&action.edits, 0, sizeof action.edits);
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_BEACON_OFFER_NAME;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "Welcome");
    action.edits[1].field = MESH_UI_FIELD_BEACON_OFFER_KEY;
    action.edits[1].number = (uint32_t)MESH_UI_PSK_RANDOM_128;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          !beacon->has_broadcast_offer_channel ||
                          beacon->broadcast_offer_channel.psk.size != 16U,
                      "a random AES-128 offered key should be sixteen bytes");
    record_success(test_name);
}

/*
 * Emptying the offered channel's name takes the whole invitation with it, in either order.
 *
 * The row's note promises that an empty name offers no channel, and a promise the screen makes
 * is kept against the record the save assembles rather than against one edit: a save carries
 * several edits in whatever order they were made, so a name cleared before the key was touched
 * has to mean the same as one cleared after it. Left to the row's own arm it would not - the
 * key arm would put `has_broadcast_offer_channel` back and the radio would go on broadcasting a
 * nameless invitation with a live key behind it.
 */
MESH_TEST_CASE(radio_settings_write_drops_a_nameless_beacon_offer, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    radio.has_mesh_beacon = true;
    radio.mesh_beacon.has_broadcast_offer_channel = true;
    snprintf(radio.mesh_beacon.broadcast_offer_channel.name,
             sizeof radio.mesh_beacon.broadcast_offer_channel.name, "Welcome");
    radio.mesh_beacon.broadcast_offer_channel.psk.size = 16U;
    radio.mesh_beacon.broadcast_offer_channel.psk.bytes[0] = 0x42U;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_BEACON;
    /* The name cleared, and a key edit *after* it - the order that would put the submessage
       back if presence were decided a row at a time. */
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_BEACON_OFFER_NAME;
    action.edits[0].text[0] = '\0';
    action.edits[1].field = MESH_UI_FIELD_BEACON_OFFER_KEY;
    action.edits[1].number = (uint32_t)MESH_UI_PSK_RANDOM_256;

    struct mesh_admin_request write;
    const meshtastic_ModuleConfig_MeshBeaconConfig *beacon =
        &write.payload.module_config.payload_variant.mesh_beacon;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0,
                      "a beacon save should build");
    MESH_TEST_FAIL_IF(beacon->has_broadcast_offer_channel,
                      "an emptied offer name should take the whole submessage with it");
    MESH_TEST_FAIL_IF(beacon->broadcast_offer_channel.psk.size != 0U,
                      "a dropped offer must not travel as a bare key");

    /* And a name that is still there keeps it, which is what says the rule is the name rather
       than a save. */
    memset(&action.edits, 0, sizeof action.edits);
    action.edit_count = 1U;
    action.edits[0].field = MESH_UI_FIELD_BEACON_INTERVAL;
    action.edits[0].number = 7200U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          !beacon->has_broadcast_offer_channel ||
                          strcmp(beacon->broadcast_offer_channel.name, "Welcome") != 0,
                      "a save touching neither offer row should leave the invitation alone");
    record_success(test_name);
}

MESH_TEST_CASE(radio_settings_write_preserves_unshown_fields, unit) {
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    struct mesh_admin_request write;

    /* LoRa: the advanced group, which now has rows and is edited by none of them here - and
       pa_fan_disabled, which has no row at all. */
    radio.has_lora = true;
    radio.lora.hop_limit = 3U;
    radio.lora.override_frequency = 906.875f;
    radio.lora.frequency_offset = 1.5f;
    radio.lora.channel_num = 42U;
    radio.lora.override_duty_cycle = true;
    radio.lora.sx126x_rx_boosted_gain = true;
    radio.lora.pa_fan_disabled = true;
    radio.lora.ignore_incoming_count = 2U;
    radio.lora.ignore_incoming[0] = 0xDEADBEEFU;
    radio.lora.ignore_incoming[1] = 0x0000BEEFU;
    action.section = MESH_UI_SETTINGS_LORA;
    action.edit_count = 1U;
    action.edits[0].field = MESH_UI_FIELD_LORA_HOPS;
    action.edits[0].number = 5U;
    const meshtastic_Config_LoRaConfig *lora = &write.payload.config.payload_variant.lora;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          lora->hop_limit != 5U || lora->override_frequency != 906.875f ||
                          lora->frequency_offset != 1.5f || lora->channel_num != 42U ||
                          !lora->override_duty_cycle || !lora->sx126x_rx_boosted_gain ||
                          !lora->pa_fan_disabled || lora->ignore_incoming_count != 2U ||
                          lora->ignore_incoming[0] != 0xDEADBEEFU ||
                          lora->ignore_incoming[1] != 0x0000BEEFU,
                      "a LoRa save must keep the frequency, duty cycle and ignore list");

    /* Position: position_flags now has ten rows over it, and none of them was pressed - so the
       word has to come back exactly as the radio reported it. The bit arithmetic itself is
       radio_settings_write_sets_one_position_flag below. */
    radio.has_position = true;
    radio.position.position_flags = 0x0000030FU;
    radio.position.rx_gpio = 17U;
    radio.position.gps_en_gpio = 21U;
    action.section = MESH_UI_SETTINGS_POSITION;
    action.edit_count = 1U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_POSITION_BROADCAST_SECS;
    action.edits[0].number = 900U;
    const meshtastic_Config_PositionConfig *pos = &write.payload.config.payload_variant.position;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          pos->position_broadcast_secs != 900U ||
                          pos->position_flags != 0x0000030FU || pos->rx_gpio != 17U ||
                          pos->gps_en_gpio != 21U,
                      "a Position save must keep the position flags and the GPS pins");

    /* Power: sds_secs and the battery calibration pair. */
    radio.has_power = true;
    radio.power.sds_secs = 604800U;
    radio.power.adc_multiplier_override = 2.11f;
    radio.power.device_battery_ina_address = 0x40U;
    action.section = MESH_UI_SETTINGS_POWER;
    action.edit_count = 1U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_POWER_MIN_WAKE;
    action.edits[0].number = 60U;
    const meshtastic_Config_PowerConfig *power = &write.payload.config.payload_variant.power;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          power->min_wake_secs != 60U || power->sds_secs != 604800U ||
                          power->adc_multiplier_override != 2.11f ||
                          power->device_battery_ina_address != 0x40U,
                      "a Power save must keep the deep sleep timer and the battery calibration");

    /* Display: the two fields that stay unshown now the other six have rows. */
    radio.has_display = true;
    radio.display.displaymode = meshtastic_Config_DisplayConfig_DisplayMode_TWOCOLOR;
    radio.display.wake_on_tap_or_motion = true;
    radio.display.compass_north_top = true; /* deprecated upstream, and still the radio's */
    action.section = MESH_UI_SETTINGS_DISPLAY;
    action.edit_count = 1U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_DISPLAY_HEADING_BOLD;
    action.edits[0].number = 1U;
    const meshtastic_Config_DisplayConfig *disp = &write.payload.config.payload_variant.display;
    MESH_TEST_FAIL_IF(
        mesh_app_build_settings_write(&radio, &action, &write) != 0 || !disp->heading_bold ||
            disp->displaymode != meshtastic_Config_DisplayConfig_DisplayMode_TWOCOLOR ||
            !disp->wake_on_tap_or_motion || !disp->compass_north_top,
        "a Display save must keep what it does not offer");

    /* The radio's own screen: the interlude's reason for keeping DeviceUIConfig whole. A write
       assembled from the rows alone would erase a touchscreen's calibration. */
    radio.has_ui_config = true;
    radio.ui_config.screen_brightness = 80U;
    radio.ui_config.screen_rgb_color = 0x00FF7700U;
    radio.ui_config.calibration_data.size = 4U;
    radio.ui_config.calibration_data.bytes[0] = 0xA5U;
    radio.ui_config.has_node_filter = true;
    radio.ui_config.node_filter.hops_away = 3;
    action.section = MESH_UI_SETTINGS_RADIO_UI;
    action.edit_count = 1U;
    memset(action.edits, 0, sizeof action.edits);
    action.edits[0].field = MESH_UI_FIELD_UI_BRIGHTNESS;
    action.edits[0].number = 128U;
    MESH_TEST_FAIL_IF(mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
                          write.payload.ui_config.screen_brightness != 128U ||
                          write.payload.ui_config.screen_rgb_color != 0x00FF7700U ||
                          write.payload.ui_config.calibration_data.size != 4U ||
                          write.payload.ui_config.calibration_data.bytes[0] != 0xA5U ||
                          !write.payload.ui_config.has_node_filter ||
                          write.payload.ui_config.node_filter.hops_away != 3,
                      "a Radio UI save must keep the calibration, colour and node filter");

    record_success(test_name);
}

/*
 * A protobuf float is four bytes of whatever arrived, and casting one to an integer is only
 * defined for a finite value inside the target's range.
 *
 * NaN, either infinity and an absurd magnitude all reach mesh_app_scale_float() from a
 * malformed or corrupted LoRa config, where nothing upstream of it has looked at the bits.
 * Under UBSan the cast is a crash rather than a wrong number, which is what makes this worth a
 * case rather than a comment: the failure it prevents is a client that falls over while
 * drawing a settings row.
 *
 * Refused reads as 0, and nothing is lost by that. A row nobody edited is never written back
 * from this side - a save starts from the radio's own record - so the garbage stays on the
 * radio rather than being laundered into a number this client invented.
 */
MESH_TEST_CASE(app_scale_float_refuses_what_it_cannot_cast, unit) {
    /* The ordinary case still works, and round-trips: this is a guard, not a clamp on values
       anybody has. */
    const int64_t scaled = mesh_app_scale_float(906.875f, MESH_UI_FREQUENCY_DIGITS);
    MESH_TEST_FAIL_IF(scaled != 9068750, "a real frequency should scale exactly");
    MESH_TEST_FAIL_IF(mesh_app_unscale_float(scaled, MESH_UI_FREQUENCY_DIGITS) != 906.875f,
                      "and come back as the float it started as");
    MESH_TEST_FAIL_IF(mesh_app_scale_float(-12.5f, MESH_UI_HERTZ_DIGITS) != -125,
                      "a negative trim should scale");

    /* Built rather than written as literals, so no constant folding decides the answer at
       compile time and the cast is the one the radio's bytes would reach. */
    volatile float zero = 0.0f;
    volatile float huge = 3.0e38f;
    const float nan = zero / zero;
    const float positive_infinity = huge * huge;
    const float negative_infinity = -positive_infinity;

    MESH_TEST_FAIL_IF(mesh_app_scale_float(nan, MESH_UI_FREQUENCY_DIGITS) != 0,
                      "a NaN frequency is not a number and must not be cast to one");
    MESH_TEST_FAIL_IF(mesh_app_scale_float(positive_infinity, MESH_UI_FREQUENCY_DIGITS) != 0 ||
                          mesh_app_scale_float(negative_infinity, MESH_UI_FREQUENCY_DIGITS) != 0,
                      "neither infinity may be cast");
    /* Finite, and still past what an int64 holds once the scale is applied. */
    MESH_TEST_FAIL_IF(mesh_app_scale_float(1.0e30f, MESH_UI_FREQUENCY_DIGITS) != 0 ||
                          mesh_app_scale_float(-1.0e30f, MESH_UI_HERTZ_DIGITS) != 0,
                      "a magnitude past the cast's range is refused rather than wrapped");
    record_success(test_name);
}

/*
 * Resolving a relay byte to a name, and declining to.
 *
 * MeshPacket.relay_node and .next_hop carry the *last byte* of a node number, because that is
 * all the LoRa header has room for. So the lookup is not a lookup: a byte matches one node
 * number in 256, and a mesh of a hundred nodes therefore collides by arithmetic rather than by
 * bad luck. The rule this pins is that a name is drawn only when the roster has exactly one
 * candidate, and that everything else - no candidate, two candidates - falls back to the
 * "!..a3" partial id, which is the honest rendering of two hex digits.
 *
 * Getting this wrong is not a cosmetic bug. "Relayed by ALICE" against a node that did not
 * relay it is the client inventing a path through the mesh, and the reader has no way to tell
 * that from one it measured.
 *
 * The sender shortcut is the subtle half. A node stamps itself into relay_node as it transmits,
 * so a byte matching the sender normally means nothing carried the packet - but a packet that
 * came at least one hop *was* carried, and there the match is a collision. The hop count is the
 * evidence that tells those apart, and without it the shortcut hides the one relay the reader
 * could have been told about.
 */
MESH_TEST_CASE(app_relay_name_declines_to_guess, unit) {
    struct mesh_handshake_status status;
    memset(&status, 0, sizeof status);
    status.node_count = 3U;
    status.nodes[0].node_id = 0xAAAA0055U;
    snprintf(status.nodes[0].short_name, sizeof status.nodes[0].short_name, "RLAY");
    status.nodes[1].node_id = 0xBBBB0077U;
    snprintf(status.nodes[1].short_name, sizeof status.nodes[1].short_name, "TWIN");
    /* The collision: a different node whose number ends in the same byte as node[1]'s. */
    status.nodes[2].node_id = 0xCCCC0077U;
    snprintf(status.nodes[2].short_name, sizeof status.nodes[2].short_name, "ALSO");

    char name[16];
    const int unknown_hops = -1;

    /* One candidate: the name, and this is the whole point of the feature. */
    mesh_app_format_relay_name(&status, 0x55U, 0U, unknown_hops, name, sizeof name);
    MESH_TEST_FAIL_IF(strcmp(name, "RLAY") != 0, "an unambiguous byte should name its node");

    /* Two candidates: neither, because naming either would be a coin toss drawn as a fact. */
    mesh_app_format_relay_name(&status, 0x77U, 0U, unknown_hops, name, sizeof name);
    MESH_TEST_FAIL_IF(strcmp(name, "!..77") != 0, "an ambiguous byte named one of its candidates");

    /* None: the byte, rather than silence - the radio did tell us something. */
    mesh_app_format_relay_name(&status, 0x12U, 0U, unknown_hops, name, sizeof name);
    MESH_TEST_FAIL_IF(strcmp(name, "!..12") != 0, "an unmatched byte should still say what it is");

    /* Nothing to say, twice over. Zero is upstream's NO_RELAY_NODE, and a byte that is the
       sender's own is the firmware saying nothing relayed this - the overwhelmingly common
       case on a small mesh, and a chip on every bubble if it were not filtered here. */
    mesh_app_format_relay_name(&status, 0U, 0xAAAA0055U, unknown_hops, name, sizeof name);
    MESH_TEST_FAIL_IF(name[0] != '\0', "a zero relay byte is 'the firmware did not say'");
    mesh_app_format_relay_name(&status, 0x55U, 0xAAAA0055U, unknown_hops, name, sizeof name);
    MESH_TEST_FAIL_IF(name[0] != '\0', "a packet heard straight from its sender names no relay");

    /* Zero hops is the firmware *saying* direct, which is the same answer and not the same
       thing as it declining to say. Both leave the shortcut standing. */
    mesh_app_format_relay_name(&status, 0x55U, 0xAAAA0055U, 0, name, sizeof name);
    MESH_TEST_FAIL_IF(name[0] != '\0', "a zero-hop packet came straight from its sender");

    /*
     * And the case the shortcut used to swallow: the packet came a hop, so something *did*
     * carry it, and a byte matching the sender is a collision with whatever that was. Saying
     * nothing here hid a relay the reader could have been told about.
     *
     * The answer is the partial id and specifically not "RLAY", which is the sender's own name
     * and the only node in this roster ending in 0x55: a node cannot have relayed a packet it
     * sent, so it is struck off the candidates rather than left in to be named.
     */
    mesh_app_format_relay_name(&status, 0x55U, 0xAAAA0055U, 2, name, sizeof name);
    MESH_TEST_FAIL_IF(strcmp(name, "!..55") != 0,
                      "a relayed packet whose relay byte matches its sender is a collision");

    /* The sender test is on the byte, not the node: a relay that merely shares the sender's
       last byte is indistinguishable from the sender while nothing says it travelled. */
    mesh_app_format_relay_name(&status, 0x55U, 0xDDDD0055U, unknown_hops, name, sizeof name);
    MESH_TEST_FAIL_IF(name[0] != '\0', "a byte matching the sender's cannot be read as a relay");

    /* A roster we do not have yet is not a reason to say nothing: the byte still stands. */
    mesh_app_format_relay_name(NULL, 0x55U, 0U, unknown_hops, name, sizeof name);
    MESH_TEST_FAIL_IF(strcmp(name, "!..55") != 0, "no roster should still render the byte");

    /* The same roster read as the ambiguity question the detail screen is handed, because it
       cannot ask it of the 128 nodes it gets. 0x77 has two claimants; 0x55 has one. */
    MESH_TEST_FAIL_IF(!mesh_app_relay_byte_is_ambiguous(&status, 0x77U),
                      "two nodes ending in 0x77 is the definition of ambiguous");
    MESH_TEST_FAIL_IF(mesh_app_relay_byte_is_ambiguous(&status, 0x55U) ||
                          mesh_app_relay_byte_is_ambiguous(&status, 0x12U),
                      "one claimant, or none, is not ambiguous");

    record_success(test_name);
}

/*
 * The ambiguity a screen cannot see, which is the reason it is answered at publish at all.
 *
 * The session holds MESH_SESSION_MAX_NODES and the UI is published MESH_UI_MAX_HANDSHAKE_NODES
 * of them, ranked. So on a mesh past that cut a relay byte can have exactly one claimant among
 * the nodes a screen was handed and another one that was ranked away - and a resolver scanning
 * only what it was given would name that survivor and sound certain. Nothing about the published
 * roster can detect this; it has to be settled where the whole roster is.
 */
MESH_TEST_CASE(app_relay_ambiguity_spans_the_whole_roster, unit) {
    static struct mesh_handshake_status status;
    memset(&status, 0, sizeof status);

    /* A roster past the publish cut, every node ending in a distinct byte except the pair
       below, so nothing else in it colours the answer. */
    status.node_count = MESH_UI_MAX_HANDSHAKE_NODES + 8U;
    MESH_TEST_FAIL_IF(status.node_count > MESH_SESSION_MAX_NODES, "fixture outgrew the roster");
    for (size_t i = 0; i < status.node_count; ++i) {
        status.nodes[i].node_id = 0x00010000U + (uint32_t)(i * 0x100U);
    }

    /* The colliding pair: one inside the published 128, one past it. */
    status.nodes[0].node_id = 0x0A0A00C3U;
    snprintf(status.nodes[0].short_name, sizeof status.nodes[0].short_name, "NEAR");
    status.nodes[status.node_count - 1U].node_id = 0x0B0B00C3U;
    snprintf(status.nodes[status.node_count - 1U].short_name,
             sizeof status.nodes[status.node_count - 1U].short_name, "FARR");

    MESH_TEST_FAIL_IF(!mesh_app_relay_byte_is_ambiguous(&status, 0xC3U),
                      "a claimant past the publish cut still makes the byte ambiguous");
    char name[16];
    mesh_app_format_relay_name(&status, 0xC3U, 0U, -1, name, sizeof name);
    MESH_TEST_FAIL_IF(strcmp(name, "!..c3") != 0,
                      "the core resolver reads the whole roster, so it refuses to name NEAR");

    record_success(test_name);
}

static int app_verify_sink(void *ctx, const uint8_t *packet, size_t len, uint32_t packet_id) {
    (void)ctx;
    (void)packet;
    (void)len;
    (void)packet_id;
    return 0;
}

/*
 * Every press on "Verify this key" puts the waiting sheet up, including one on an exchange that
 * is still waiting. B closes that sheet without ending the exchange, so the next press starts
 * over in the very stage the sheet last showed - and the publish, which opens only on a stage it
 * has not shown, left the user with a toast and no sheet until the old exchange ran out.
 */
MESH_TEST_CASE(app_verify_press_reopens_a_dismissed_waiting_sheet, unit) {
    const char *failure = NULL;
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    struct mesh_bluez_mock_config mock_config = {.adapter_path = "/org/bluez/hci0"};
    mesh_bluez_client_mock_enable(&mock_config);

    char home_dir[APP_TEST_HOME_CAP];
    if (!app_test_home(home_dir, sizeof home_dir, "verify")) {
        failure = "mkdtemp failed";
        goto cleanup;
    }
    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    mesh_session_attach(&app.session, app_verify_sink, NULL);
    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = 0x1000U;
    (void)mesh_test_session_feed_from_radio(&app.session, &my_info);
    meshtastic_FromRadio info = meshtastic_FromRadio_init_default;
    info.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    info.node_info.num = 0x2001U;
    info.node_info.has_user = true;
    snprintf(info.node_info.user.long_name, sizeof info.node_info.user.long_name, "Pine Ridge");
    info.node_info.user.public_key.size = 32U;
    info.node_info.user.public_key.bytes[0] = 0xA1U;
    (void)mesh_test_session_feed_from_radio(&app.session, &info);

    struct mesh_ui_action press;
    memset(&press, 0, sizeof press);
    press.type = MESH_UI_ACTION_VERIFY_KEY;
    press.dest = 0x2001U;

    mesh_app_on_ui_action(&app, &press);
    mesh_app_publish_ui_state(&app);
    if (!app.ui_store.nav.verify_open) {
        failure = "the first press did not put the waiting sheet up";
        goto cleanup;
    }

    /* B: the sheet goes, the exchange stays. */
    mesh_ui_store_close_verify_sheet(&app.ui_store);
    mesh_app_publish_ui_state(&app);
    if (app.ui_store.nav.verify_open) {
        failure = "a dismissed waiting sheet came back with nothing new to say";
        goto cleanup;
    }

    /* The first INITIATE has gone out, as it has on a radio by now; a step still queued would
       refuse the press as a double press instead. */
    app.session.settings.queue_len = 0U;
    mesh_app_on_ui_action(&app, &press);
    mesh_app_publish_ui_state(&app);
    if (!app.ui_store.nav.verify_open) {
        failure = "a second press on a waiting exchange did not put the sheet back";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
