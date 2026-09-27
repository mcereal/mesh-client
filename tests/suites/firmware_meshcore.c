/*
 * The MeshCore half of the firmware catalog, against the flasher's own documents as captured in
 * tests/data/ - see the README there for what each was cut to.
 */

#include "framework/mesh_test.h"
#include "support/data_fixture.h"

#include "mesh/core/firmware_meshcore.h"

#include <stdlib.h>
#include <string.h>

static bool boards_for(const char *model, bool usb, struct mesh_firmware_boards *boards) {
    size_t len = 0U;
    char *document = mesh_test_data_read("meshcore_flasher_config.json", &len);
    if (document == NULL) {
        return false;
    }
    const bool parsed = mesh_firmware_meshcore_boards_parse(document, len, model, usb, boards);
    free(document);
    return parsed;
}

/* The test radio: DEVICE_INFO says "Heltec V3", the flasher says "Heltec v3". */
MESH_TEST_CASE(firmware_meshcore_names_the_build_a_companion_runs, unit) {
    struct mesh_firmware_boards boards;
    MESH_TEST_FAIL_IF(!boards_for("Heltec V3", true, &boards), "the flasher's list should parse");
    MESH_TEST_FAIL_IF(boards.found != 1U, "one USB companion build for the Heltec V3");
    const struct mesh_firmware_board *const v3 = &boards.entries[0];
    MESH_TEST_FAIL_IF(strcmp(v3->target, "Heltec_v3_companion_radio_usb") != 0,
                      "the build's own name, which is how its files start");
    MESH_TEST_FAIL_IF(strcmp(v3->name, "Heltec v3") != 0, "the flasher's name for the device");
    MESH_TEST_FAIL_IF(strcmp(v3->architecture, "esp32") != 0 ||
                          !mesh_firmware_architecture_uses_esp_rom(v3->architecture),
                      "the ESP32 family, which goes through its ROM");
    MESH_TEST_FAIL_IF(v3->path != MESH_FIRMWARE_PATH_USB || !v3->actively_supported, "over USB");

    MESH_TEST_FAIL_IF(!boards_for("Heltec V3", false, &boards) || boards.found != 1U ||
                          strcmp(boards.entries[0].target, "Heltec_v3_companion_radio_ble") != 0,
                      "on BLE it is the BLE companion build");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_meshcore_matches_through_the_alias_and_never_guesses, unit) {
    struct mesh_firmware_boards boards;
    MESH_TEST_FAIL_IF(!boards_for("RAK 4631", true, &boards) || boards.found != 1U,
                      "\"RAK 4631\" is the flasher's \"RAK WisBlock / WisMesh (RAK 4631)\"");
    MESH_TEST_FAIL_IF(strcmp(boards.entries[0].target, "RAK_4631_companion_radio_usb") != 0 ||
                          strcmp(boards.entries[0].architecture, "nrf52840") != 0,
                      "an nRF52 build");
    MESH_TEST_FAIL_IF(boards.entries[0].path != MESH_FIRMWARE_PATH_USB ||
                          !mesh_firmware_board_takes(&boards.entries[0], MESH_FIRMWARE_PATH_USB),
                      "over USB, through the bootloader a 1200-baud touch reaches");
    MESH_TEST_FAIL_IF(!boards.entries[0].meshcore ||
                          mesh_firmware_board_takes(&boards.entries[0], MESH_FIRMWARE_PATH_BLE),
                      "and never over BLE, where Nordic DFU arms a radio by asking it");

    /* One device, two builds - with a display and without - and the radio's name does not
       say which. */
    MESH_TEST_FAIL_IF(!boards_for("Heltec T114", true, &boards) || boards.found != 2U,
                      "the T114 is two candidates, never the first of them");

    MESH_TEST_FAIL_IF(!boards_for("Heltec V9", true, &boards) || boards.found != 0U,
                      "a name nobody publishes for is no board");
    MESH_TEST_FAIL_IF(!boards_for("Heltec", true, &boards) || boards.found != 0U,
                      "and a name is matched whole, not as the start of another");
    MESH_TEST_FAIL_IF(
        !mesh_firmware_meshcore_names("Heltec V3", "Heltec v3") ||
            !mesh_firmware_meshcore_names("RAK 4631", "RAK WisBlock / WisMesh (RAK 4631)") ||
            mesh_firmware_meshcore_names("Heltec V4", "Heltec v3") ||
            mesh_firmware_meshcore_names("", ""),
        "the handover holds a radio to the device its image was chosen for");

    /* The same device with its keys in another order: an object's order means nothing. */
    const char *const reordered =
        "{\"device\":[{\"firmware\":[{\"role\":\"companionUsb\",\"github\":{\"files\":"
        "{\"flash-update\":\"Heltec_v3_companion_radio_usb.*?-[a-f0-9]{7}\\\\.bin\"}}}],"
        "\"type\":\"esp32\",\"name\":\"Heltec v3\"}]}";
    MESH_TEST_FAIL_IF(!mesh_firmware_meshcore_boards_parse(reordered, strlen(reordered),
                                                           "Heltec V3", true, &boards) ||
                          boards.found != 1U ||
                          strcmp(boards.entries[0].target, "Heltec_v3_companion_radio_usb") != 0,
                      "a device whose builds come before its name is still that device");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_meshcore_takes_the_newest_tag_by_version, unit) {
    size_t len = 0U;
    char *document = mesh_test_data_read("meshcore_companion_tags.json", &len);
    MESH_TEST_FAIL_IF(document == NULL, "tests/data/meshcore_companion_tags.json");
    char tag[MESH_FIRMWARE_MESHCORE_TAG_MAX];
    char version[MESH_FIRMWARE_VERSION_MAX];
    const bool found =
        mesh_firmware_meshcore_latest_tag(document, len, tag, sizeof tag, version, sizeof version);
    free(document);
    /* The list is in name order, so its last entry is companion-v1.9.0. */
    MESH_TEST_FAIL_IF(!found || strcmp(tag, "companion-v1.17.1") != 0 ||
                          strcmp(version, "1.17.1") != 0,
                      "the newest is the highest version, not the last name");

    MESH_TEST_FAIL_IF(
        mesh_firmware_meshcore_latest_tag("[]", 2U, tag, sizeof tag, version, sizeof version),
        "no tags is no release");
    const char *const others = "[{\"ref\":\"refs/tags/repeater-v1.18.0\"},"
                               "{\"ref\":\"refs/tags/companion-vX\"}]";
    MESH_TEST_FAIL_IF(mesh_firmware_meshcore_latest_tag(others, strlen(others), tag, sizeof tag,
                                                        version, sizeof version),
                      "and a repeater's tag, or one with no number, is not a companion release");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_meshcore_finds_the_one_file_an_install_writes, unit) {
    size_t len = 0U;
    char *document = mesh_test_data_read("meshcore_release_companion-v1.17.1.json", &len);
    MESH_TEST_FAIL_IF(document == NULL, "tests/data/meshcore_release_companion-v1.17.1.json");

    struct mesh_firmware_boards boards;
    struct mesh_firmware_release release;
    memset(&release, 0, sizeof release);
    bool ok = boards_for("Heltec V3", true, &boards) &&
              mesh_firmware_meshcore_asset_parse(document, len, &boards.entries[0], &release);
    MESH_TEST_FAIL_IF_CLEANUP(!ok, free(document), "the V3's USB build is in the release");
    MESH_TEST_FAIL_IF_CLEANUP(
        strcmp(release.image_name, "Heltec_v3_companion_radio_usb-v1.17.1-d929643.bin") != 0,
        free(document), "the app image, not the -merged one that would overwrite the bootloader");
    MESH_TEST_FAIL_IF_CLEANUP(release.image_bytes != 644000U ||
                                  strstr(release.image_url,
                                         "/download/companion-v1.17.1/Heltec_v3_companion_radio_"
                                         "usb-v1.17.1-d929643.bin") == NULL,
                              free(document), "with its size and where to get it");

    /* The RAK's is the .uf2, not the DFU .zip beside it. */
    ok = boards_for("RAK 4631", false, &boards) &&
         mesh_firmware_meshcore_asset_parse(document, len, &boards.entries[0], &release);
    MESH_TEST_FAIL_IF_CLEANUP(
        !ok || strcmp(release.image_name, "RAK_4631_companion_radio_ble-v1.17.1-d929643.uf2") != 0,
        free(document), "an nRF52 build installs from its UF2");

    /* The T114's builds share a start: one name must not find the other's file. */
    ok = boards_for("Heltec T114", true, &boards) &&
         mesh_firmware_meshcore_asset_parse(document, len, &boards.entries[0], &release);
    MESH_TEST_FAIL_IF_CLEANUP(!ok || strstr(release.image_name, "without_display") != NULL,
                              free(document), "the build with a display finds its own file");

    struct mesh_firmware_board missing = boards.entries[0];
    memcpy(missing.target, "Heltec_v9", sizeof "Heltec_v9");
    ok = mesh_firmware_meshcore_asset_parse(document, len, &missing, &release);
    free(document);
    MESH_TEST_FAIL_IF(ok || release.image_url[0] != '\0',
                      "a build the release has no file for is no image, and says nothing");
    record_success(test_name);
}
