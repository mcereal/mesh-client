#define _POSIX_C_SOURCE 200809L

/* Preferences on disk, and the list of radios the client remembers. */

#include "framework/mesh_test.h"

#include "mesh/core/app.h"
#include "mesh/core/updater.h"
#include "mesh/ui/preferences.h"
#include "mesh/ui/store.h"
#include "mesh/ui/theme.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

MESH_TEST_CASE(ui_preferences_roundtrip, unit) {
    char prefab_path[128];
    snprintf(prefab_path, sizeof prefab_path, "/tmp/meshclient_prefs_%ld", (long)getpid());
    FILE *temp = fopen(prefab_path, "w");
    MESH_TEST_FAIL_IF(temp == NULL, "failed to create temp file");
    fclose(temp);

    struct mesh_ui_preferences prefs;
    memset(&prefs, 0, sizeof prefs);
    snprintf(prefs.preferred_device, sizeof prefs.preferred_device, "%s", "AA:BB:CC:DD:EE:01");
    snprintf(prefs.preferred_channel, sizeof prefs.preferred_channel, "%s", "LongRange");
    prefs.update_channel = (uint8_t)MESH_UPDATE_CHANNEL_PRERELEASE;
    prefs.update_allow_dev = true;
    snprintf(prefs.language, sizeof prefs.language, "%s", "es");
    snprintf(prefs.theme, sizeof prefs.theme, "%s", "light");

    if (mesh_ui_preferences_save(&prefs, prefab_path) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "save failed");
        return;
    }

    struct mesh_ui_preferences loaded;
    if (mesh_ui_preferences_load(&loaded, prefab_path) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "load failed");
        return;
    }

    if (strcmp(loaded.preferred_device, prefs.preferred_device) != 0 ||
        strcmp(loaded.preferred_channel, prefs.preferred_channel) != 0 ||
        loaded.preferred_device_kind != prefs.preferred_device_kind ||
        loaded.update_channel != prefs.update_channel ||
        loaded.update_allow_dev != prefs.update_allow_dev ||
        strcmp(loaded.language, prefs.language) != 0 || strcmp(loaded.theme, prefs.theme) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "roundtrip mismatch");
        return;
    }

    /* A theme id this build does not know is kept as written rather than corrected on load:
       the file may have been written by a version that had more themes, and it resolves to the
       default when it is looked up. Losing the name would move the user permanently. */
    snprintf(prefs.theme, sizeof prefs.theme, "%s", "solarized");
    if (mesh_ui_preferences_save(&prefs, prefab_path) != 0 ||
        mesh_ui_preferences_load(&loaded, prefab_path) != 0 ||
        strcmp(loaded.theme, "solarized") != 0) {
        unlink(prefab_path);
        record_failure(test_name, "an unknown theme id did not survive a roundtrip");
        return;
    }
    if (mesh_ui_theme_resolve(loaded.theme) != mesh_ui_theme_default()) {
        unlink(prefab_path);
        record_failure(test_name, "an unknown theme id should resolve to the default");
        return;
    }
    snprintf(prefs.theme, sizeof prefs.theme, "%s", "light");

    /* A USB port roundtrips as serial, so the reconnect goes to the right link. */
    snprintf(prefs.preferred_device, sizeof prefs.preferred_device, "%s", "/dev/ttyUSB0");
    prefs.preferred_device_kind = (uint8_t)MESH_UI_DEVICE_SERIAL;
    if (mesh_ui_preferences_save(&prefs, prefab_path) != 0 ||
        mesh_ui_preferences_load(&loaded, prefab_path) != 0 ||
        loaded.preferred_device_kind != (uint8_t)MESH_UI_DEVICE_SERIAL ||
        strcmp(loaded.preferred_device, "/dev/ttyUSB0") != 0) {
        unlink(prefab_path);
        record_failure(test_name, "serial preference did not roundtrip");
        return;
    }

    /* A file from before the kind was recorded: a tty path must not be handed to BLE. */
    temp = fopen(prefab_path, "w");
    if (temp == NULL) {
        unlink(prefab_path);
        record_failure(test_name, "failed to rewrite temp file");
        return;
    }
    /* No update_channel line: a file written before the setting existed must read as DEFAULT,
       so the updater keeps inferring the channel from the build rather than being moved. */
    fprintf(temp, "preferred_device=/dev/ttyUSB0\npreferred_channel=LongFast\n");
    fclose(temp);
    memset(&loaded, 0, sizeof loaded);
    if (mesh_ui_preferences_load(&loaded, prefab_path) != 0 ||
        loaded.preferred_device_kind != (uint8_t)MESH_UI_DEVICE_SERIAL) {
        unlink(prefab_path);
        record_failure(test_name, "a legacy tty preference should migrate to serial");
        return;
    }
    if (loaded.update_channel != (uint8_t)MESH_UPDATE_CHANNEL_DEFAULT || loaded.update_allow_dev) {
        unlink(prefab_path);
        record_failure(test_name, "a file without the settings should read as the defaults");
        return;
    }

    unlink(prefab_path);
    record_success(test_name);
}

/*
 * The client's memory of its own radios. A favorite is stored in the connected radio's NodeDB
 * and resolved per receiver, so pins never follow the Brick from one of your nodes to another;
 * this list does, and it is what mesh_app_node_rank() keeps the node you unplugged with.
 */
MESH_TEST_CASE(ui_preferences_known_radios, unit) {
    struct mesh_ui_preferences prefs;
    memset(&prefs, 0, sizeof prefs);

    if (mesh_ui_preferences_note_radio(&prefs, 0U) || mesh_ui_preferences_knows_radio(&prefs, 0U)) {
        record_failure(test_name, "node 0 is not a node");
        return;
    }
    MESH_TEST_FAIL_IF(!mesh_ui_preferences_note_radio(&prefs, 0xABC123U) ||
                          prefs.known_radio_count != 1U ||
                          !mesh_ui_preferences_knows_radio(&prefs, 0xABC123U),
                      "the first radio should be recorded");
    /* Every publish notes the connected radio again; only a change is worth a file write. */
    MESH_TEST_FAIL_IF(mesh_ui_preferences_note_radio(&prefs, 0xABC123U) ||
                          prefs.known_radio_count != 1U,
                      "re-noting the most recent radio should not dirty the file");
    MESH_TEST_FAIL_IF(!mesh_ui_preferences_note_radio(&prefs, 0xDEF456U) ||
                          prefs.known_radio_count != 2U || prefs.known_radios[0] != 0xDEF456U ||
                          prefs.known_radios[1] != 0xABC123U,
                      "the newly connected radio should lead, the old one survive");
    /* Switching back moves it to the front rather than adding it twice. */
    MESH_TEST_FAIL_IF(!mesh_ui_preferences_note_radio(&prefs, 0xABC123U) ||
                          prefs.known_radio_count != 2U || prefs.known_radios[0] != 0xABC123U ||
                          prefs.known_radios[1] != 0xDEF456U,
                      "an already-known radio should move to the front");

    /* Past the cap the oldest radio falls off; the recent ones stay. */
    for (uint32_t i = 0; i < MESH_UI_MAX_KNOWN_RADIOS; ++i) {
        mesh_ui_preferences_note_radio(&prefs, 0x9000U + i);
    }
    if (prefs.known_radio_count != MESH_UI_MAX_KNOWN_RADIOS ||
        mesh_ui_preferences_knows_radio(&prefs, 0xDEF456U) ||
        !mesh_ui_preferences_knows_radio(&prefs, 0x9000U + MESH_UI_MAX_KNOWN_RADIOS - 1U)) {
        record_failure(test_name,
                       "the list should cap at MESH_UI_MAX_KNOWN_RADIOS, oldest first out");
        return;
    }

    char prefab_path[128];
    snprintf(prefab_path, sizeof prefab_path, "/tmp/meshclient_radios_%ld", (long)getpid());
    if (mesh_ui_preferences_save(&prefs, prefab_path) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "save failed");
        return;
    }
    struct mesh_ui_preferences loaded;
    if (mesh_ui_preferences_load(&loaded, prefab_path) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "load failed");
        return;
    }
    if (loaded.known_radio_count != prefs.known_radio_count ||
        memcmp(loaded.known_radios, prefs.known_radios, sizeof prefs.known_radios) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "the radio list should roundtrip in order");
        return;
    }

    /* A file written before this existed simply has no radios, not a broken parse. */
    FILE *legacy = fopen(prefab_path, "w");
    if (legacy == NULL) {
        unlink(prefab_path);
        record_failure(test_name, "failed to rewrite temp file");
        return;
    }
    fprintf(legacy, "preferred_device=AA:BB:CC:DD:EE:01\npreferred_channel=LongFast\n");
    fclose(legacy);
    memset(&loaded, 0, sizeof loaded);
    if (mesh_ui_preferences_load(&loaded, prefab_path) != 0 || loaded.known_radio_count != 0U) {
        unlink(prefab_path);
        record_failure(test_name, "a file from before known_radios should load with none");
        return;
    }

    unlink(prefab_path);
    record_success(test_name);
}

/* The list of radios this client has connected to over BLE or USB: the order is the value, and
   auto-connect reads it to answer "which of mine is in earshot" rather than "which is loudest". */
MESH_TEST_CASE(ui_preferences_known_devices, unit) {
    struct mesh_ui_preferences prefs;
    memset(&prefs, 0, sizeof prefs);

    if (mesh_ui_preferences_device_rank(&prefs, "AA:BB:CC:DD:EE:01", (uint8_t)MESH_UI_DEVICE_BLE) !=
        -1) {
        record_failure(test_name, "an empty list should know nothing");
        return;
    }

    (void)mesh_ui_preferences_note_device(&prefs, "AA:BB:CC:DD:EE:01", (uint8_t)MESH_UI_DEVICE_BLE);
    (void)mesh_ui_preferences_note_device(&prefs, "/dev/ttyUSB0", (uint8_t)MESH_UI_DEVICE_SERIAL);
    (void)mesh_ui_preferences_note_device(&prefs, "AA:BB:CC:DD:EE:02", (uint8_t)MESH_UI_DEVICE_BLE);

    /* Most recent first, and the head is preferred_device: the two are one fact. */
    if (prefs.known_device_count != 3U ||
        mesh_ui_preferences_device_rank(&prefs, "AA:BB:CC:DD:EE:02", (uint8_t)MESH_UI_DEVICE_BLE) !=
            0 ||
        mesh_ui_preferences_device_rank(&prefs, "/dev/ttyUSB0", (uint8_t)MESH_UI_DEVICE_SERIAL) !=
            1 ||
        mesh_ui_preferences_device_rank(&prefs, "AA:BB:CC:DD:EE:01", (uint8_t)MESH_UI_DEVICE_BLE) !=
            2 ||
        strcmp(prefs.preferred_device, "AA:BB:CC:DD:EE:02") != 0) {
        record_failure(test_name, "the list should be most recent first");
        return;
    }

    /* The kind is part of the identity: the same string over the other link is another device,
       which is what stops a tty path being handed to BLE. */
    if (mesh_ui_preferences_device_rank(&prefs, "/dev/ttyUSB0", (uint8_t)MESH_UI_DEVICE_BLE) !=
        -1) {
        record_failure(test_name, "the transport should be part of a device's identity");
        return;
    }

    /* An address is a BLE address however BlueZ or a config file happened to case it. */
    if (mesh_ui_preferences_device_rank(&prefs, "aa:bb:cc:dd:ee:01", (uint8_t)MESH_UI_DEVICE_BLE) !=
        2) {
        record_failure(test_name, "a BLE address should match without regard to case");
        return;
    }

    /* Coming back to an older radio moves it to the front rather than adding a second entry. */
    (void)mesh_ui_preferences_note_device(&prefs, "AA:BB:CC:DD:EE:01", (uint8_t)MESH_UI_DEVICE_BLE);
    if (prefs.known_device_count != 3U ||
        mesh_ui_preferences_device_rank(&prefs, "AA:BB:CC:DD:EE:01", (uint8_t)MESH_UI_DEVICE_BLE) !=
            0 ||
        mesh_ui_preferences_device_rank(&prefs, "AA:BB:CC:DD:EE:02", (uint8_t)MESH_UI_DEVICE_BLE) !=
            1) {
        record_failure(test_name, "reconnecting should move a radio to the front");
        return;
    }

    /* Only the handful you own is kept; the oldest falls off the end. */
    for (unsigned i = 0; i < MESH_UI_MAX_KNOWN_DEVICES; ++i) {
        char address[32];
        snprintf(address, sizeof address, "AA:BB:CC:DD:FF:%02u", i);
        (void)mesh_ui_preferences_note_device(&prefs, address, (uint8_t)MESH_UI_DEVICE_BLE);
    }
    if (prefs.known_device_count != MESH_UI_MAX_KNOWN_DEVICES ||
        mesh_ui_preferences_device_rank(&prefs, "AA:BB:CC:DD:EE:01", (uint8_t)MESH_UI_DEVICE_BLE) !=
            -1) {
        record_failure(test_name, "the list should be bounded, oldest first out");
        return;
    }

    char prefab_path[128];
    snprintf(prefab_path, sizeof prefab_path, "/tmp/meshclient_devices_%ld", (long)getpid());
    if (mesh_ui_preferences_save(&prefs, prefab_path) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "save failed");
        return;
    }

    struct mesh_ui_preferences loaded;
    if (mesh_ui_preferences_load(&loaded, prefab_path) != 0 ||
        loaded.known_device_count != prefs.known_device_count) {
        unlink(prefab_path);
        record_failure(test_name, "load failed");
        return;
    }
    for (uint8_t i = 0; i < prefs.known_device_count; ++i) {
        if (strcmp(loaded.known_devices[i].identifier, prefs.known_devices[i].identifier) != 0 ||
            loaded.known_devices[i].kind != prefs.known_devices[i].kind) {
            unlink(prefab_path);
            record_failure(test_name, "the order is the value and must survive the file");
            return;
        }
    }

    /* Forgetting a pairing drops the radio and hands the head to the one used before it. */
    if (!mesh_ui_preferences_forget_device(&loaded, loaded.known_devices[0].identifier,
                                           loaded.known_devices[0].kind)) {
        unlink(prefab_path);
        record_failure(test_name, "forget should report the device it removed");
        return;
    }
    if (loaded.known_device_count != prefs.known_device_count - 1U ||
        strcmp(loaded.preferred_device, prefs.known_devices[1].identifier) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "forgetting the head should promote the next radio");
        return;
    }

    /* A file written before the list existed carries one radio in preferred_device, and it is
       genuinely the most recent one - so the first launch after an update already knows it. */
    FILE *legacy = fopen(prefab_path, "w");
    if (legacy == NULL) {
        unlink(prefab_path);
        record_failure(test_name, "failed to rewrite temp file");
        return;
    }
    fprintf(legacy, "preferred_device=AA:BB:CC:DD:EE:09\npreferred_device_kind=ble\n");
    fclose(legacy);
    memset(&loaded, 0, sizeof loaded);
    if (mesh_ui_preferences_load(&loaded, prefab_path) != 0 || loaded.known_device_count != 1U ||
        mesh_ui_preferences_device_rank(&loaded, "AA:BB:CC:DD:EE:09",
                                        (uint8_t)MESH_UI_DEVICE_BLE) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "a file from before the list should seed it from the head");
        return;
    }

    unlink(prefab_path);
    record_success(test_name);
}
