/*
 * Reading the two documents upstream publishes, against the documents upstream published.
 *
 * Both fixtures in tests/data/ were fetched rather than written - see the README there - which
 * is the point: a parser tested against input somebody wrote to suit it is a parser tested
 * against itself. The cases below assert facts about real boards and a real release, so a
 * change in the served shape shows up here rather than on a handheld.
 */

#include "framework/mesh_test.h"
#include "support/data_fixture.h"

#include "mesh/core/firmware_catalog.h"

#include <stdlib.h>
#include <string.h>

static const struct mesh_firmware_board *board_named(const struct mesh_firmware_boards *boards,
                                                     const char *target) {
    for (uint8_t i = 0; i < boards->count; ++i) {
        if (strcmp(boards->entries[i].target, target) == 0) {
            return &boards->entries[i];
        }
    }
    return NULL;
}

/* One model, one board, and every field the row above it will show. hw_model 69 is the Heltec
   Mesh Node T114 - the board docs/radio-firmware-roadmap.md's phase 3 is written against. */
MESH_TEST_CASE(firmware_catalog_resolves_one_board, unit) {
    size_t len = 0U;
    char *document = mesh_test_data_read("device_hardware.json", &len);
    MESH_TEST_FAIL_IF(document == NULL, "tests/data/device_hardware.json should be readable");

    struct mesh_firmware_boards boards;
    const bool parsed = mesh_firmware_boards_parse(document, len, 69U, &boards);
    free(document);
    MESH_TEST_FAIL_IF(!parsed, "the hardware document should parse");
    MESH_TEST_FAIL_IF(boards.found != 1U || boards.count != 1U,
                      "hw_model 69 should resolve to exactly one board");

    const struct mesh_firmware_board *const t114 = &boards.entries[0];
    MESH_TEST_FAIL_IF(strcmp(t114->target, "heltec-mesh-node-t114") != 0,
                      "the build target names the file we would fetch");
    MESH_TEST_FAIL_IF(strcmp(t114->architecture, "nrf52840") != 0, "the T114 is an nRF52840");
    MESH_TEST_FAIL_IF(strcmp(t114->name, "Heltec Mesh Node T114") != 0,
                      "the display name is what a row would say");
    MESH_TEST_FAIL_IF(!t114->actively_supported || !t114->requires_dfu,
                      "the T114 is supported and is flashed through its bootloader");
    MESH_TEST_FAIL_IF(t114->path != MESH_FIRMWARE_PATH_USB, "an nRF52840 is the USB path");
    record_success(test_name);
}

/*
 * The case this must not get wrong.
 *
 * Nine models in the served document map to more than one board, and the widest is 48 -
 * HELTEC_WIRELESS_TRACKER - which is four. The phone app takes the first match; flashing a
 * TrackSenger image onto a Heltec Wireless Tracker is exactly the failure this feature must not
 * have, so what comes back is all four and a count, and picking one is somebody else's press.
 */
MESH_TEST_CASE(firmware_catalog_keeps_every_candidate, unit) {
    size_t len = 0U;
    char *document = mesh_test_data_read("device_hardware.json", &len);
    MESH_TEST_FAIL_IF(document == NULL, "tests/data/device_hardware.json should be readable");

    struct mesh_firmware_boards tracker;
    struct mesh_firmware_boards diy;
    struct mesh_firmware_boards unknown;
    const bool parsed = mesh_firmware_boards_parse(document, len, 48U, &tracker) &&
                        mesh_firmware_boards_parse(document, len, 39U, &diy) &&
                        mesh_firmware_boards_parse(document, len, 60000U, &unknown);
    free(document);
    MESH_TEST_FAIL_IF(!parsed, "the hardware document should parse for each lookup");

    MESH_TEST_FAIL_IF(tracker.found != 4U || tracker.count != 4U,
                      "hw_model 48 is four different boards");
    MESH_TEST_FAIL_IF(board_named(&tracker, "heltec-wireless-tracker") == NULL ||
                          board_named(&tracker, "tracksenger") == NULL ||
                          board_named(&tracker, "tracksenger-lcd") == NULL ||
                          board_named(&tracker, "tracksenger-oled") == NULL,
                      "all four candidates should be offered, not the first one");

    /* Two boards that share a model and are not even the same product: DIY V1 and Hydra. */
    MESH_TEST_FAIL_IF(diy.found != 2U, "hw_model 39 is DIY V1 and Hydra");

    /* A model nothing claims parses fine and finds nothing, which is a different row from a
       document that would not read. */
    MESH_TEST_FAIL_IF(unknown.found != 0U || unknown.count != 0U,
                      "an unknown model should resolve to no boards");
    record_success(test_name);
}

/* Which bus each architecture in the served document means, including the two that mean none. */
MESH_TEST_CASE(firmware_catalog_paths_per_architecture, unit) {
    static const struct {
        const char *architecture;
        enum mesh_firmware_path path;
    } k_expected[] = {
        {"nrf52840", MESH_FIRMWARE_PATH_USB},
        {"rp2040", MESH_FIRMWARE_PATH_USB},
        {"rp2350", MESH_FIRMWARE_PATH_USB},
        {"esp32", MESH_FIRMWARE_PATH_BLE},
        {"esp32-s3", MESH_FIRMWARE_PATH_BLE},
        {"esp32-c3", MESH_FIRMWARE_PATH_NONE},
        {"esp32-c6", MESH_FIRMWARE_PATH_NONE},
        {"portduino", MESH_FIRMWARE_PATH_NONE},
        /* The prefix trap in both directions, and an architecture that does not exist yet. */
        {"esp32-", MESH_FIRMWARE_PATH_NONE},
        {"esp32s3", MESH_FIRMWARE_PATH_NONE},
        {"nrf52", MESH_FIRMWARE_PATH_NONE},
        {"stm32wl", MESH_FIRMWARE_PATH_NONE},
        {"", MESH_FIRMWARE_PATH_NONE},
    };
    for (size_t i = 0; i < sizeof k_expected / sizeof k_expected[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_firmware_path_for_architecture(k_expected[i].architecture) !=
                              k_expected[i].path,
                          k_expected[i].architecture);
    }
    MESH_TEST_FAIL_IF(mesh_firmware_path_for_architecture(NULL) != MESH_FIRMWARE_PATH_NONE,
                      "no architecture at all is no path");
    record_success(test_name);
}

/*
 * The release index, including the three things about it that are not obvious: the notes carry
 * decoy keys, the newest alpha has published no assets at all, and an older stable's URL is a
 * per-platform zip rather than a manifest.
 */
MESH_TEST_CASE(firmware_catalog_reads_the_release_index, unit) {
    size_t len = 0U;
    char *document = mesh_test_data_read("firmware_list.json", &len);
    MESH_TEST_FAIL_IF(document == NULL, "tests/data/firmware_list.json should be readable");

    struct mesh_firmware_release stable;
    struct mesh_firmware_release alpha;
    const bool parsed =
        mesh_firmware_release_parse(document, len, MESH_FIRMWARE_CHANNEL_STABLE, &stable) &&
        mesh_firmware_release_parse(document, len, MESH_FIRMWARE_CHANNEL_ALPHA, &alpha);
    free(document);
    MESH_TEST_FAIL_IF(!parsed, "both channels should read");

    MESH_TEST_FAIL_IF(strcmp(stable.version, "2.7.26.54e0d8d") != 0,
                      "the newest stable is the first entry, without its leading v");
    MESH_TEST_FAIL_IF(strstr(stable.manifest_url, "firmware-2.7.26.54e0d8d.json") == NULL,
                      "and it should carry that release's own manifest");

    /* The decoy: the notes on both channels talk about a v9.9.9 and an example.invalid URL. */
    MESH_TEST_FAIL_IF(strstr(stable.version, "9.9.9") != NULL ||
                          strstr(stable.manifest_url, "example.invalid") != NULL,
                      "a key quoted inside a release note is not a key");

    MESH_TEST_FAIL_IF(strcmp(alpha.version, "2.8.0.47db0e3") != 0,
                      "the newest alpha is reported even though it published no assets");
    MESH_TEST_FAIL_IF(alpha.manifest_url[0] != '\0',
                      "and its manifest URL should be empty rather than the next release's");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_catalog_refuses_a_broken_index, unit) {
    static const char *const k_broken[] = {
        "",
        "not json at all",
        "{}",
        "{\"releases\": {}}",
        "{\"releases\": {\"stable\": []}}",
        "{\"releases\": {\"stable\": [{}]}}",
        "{\"releases\": {\"stable\": [{\"title\": \"no id here\"}]}}",
        "{\"releases\": {\"stable\": [{\"id\": \"v1.2.3",
        /*
         * A tag that is a 'v' and then nothing, twice over: written out, and written as the
         * escape for a NUL - which is a character the document may carry and C cannot, so the
         * string is non-empty in the reply and empty in the buffer. `make fuzz` found the
         * second one, and what it produced was a release the client would have offered as an
         * update to a version with no name.
         */
        "{\"releases\": {\"stable\": [{\"id\": \"v\"}]}}",
        "{\"releases\": {\"stable\": [{\"id\": \"v\\u0000-2.7.26\"}]}}",
    };
    for (size_t i = 0; i < sizeof k_broken / sizeof k_broken[0]; ++i) {
        struct mesh_firmware_release release;
        MESH_TEST_FAIL_IF(
            mesh_firmware_release_parse(k_broken[i], 0U, MESH_FIRMWARE_CHANNEL_STABLE, &release),
            k_broken[i][0] != '\0' ? k_broken[i] : "an empty document is not a release index");
    }

    /* A hardware document that is not an array is refused; one that is an empty array parses
       and finds nothing, which is the distinction the two rows are built on. */
    struct mesh_firmware_boards boards;
    MESH_TEST_FAIL_IF(mesh_firmware_boards_parse("{\"boards\": []}", 0U, 69U, &boards),
                      "the hardware document is a list, and anything else is unreadable");
    MESH_TEST_FAIL_IF(!mesh_firmware_boards_parse("[]", 0U, 69U, &boards) || boards.found != 0U,
                      "an empty list reads, and holds no board");
    record_success(test_name);
}

/*
 * Ordering. Meshtastic versions are `major.minor.patch.hash` and only the first three of those
 * are ordered - so two builds of the same three numbers compare equal however their hashes
 * sort. That is the answer that refuses to offer an update rather than the one that invents a
 * direction.
 */
MESH_TEST_CASE(firmware_catalog_orders_versions, unit) {
    MESH_TEST_FAIL_IF(mesh_firmware_version_compare("2.7.25.abc1234", "2.7.26.54e0d8d") >= 0,
                      "a lower patch is older");
    MESH_TEST_FAIL_IF(mesh_firmware_version_compare("2.8.0.47db0e3", "2.7.26.54e0d8d") <= 0,
                      "a higher minor is newer even with a lower patch");
    MESH_TEST_FAIL_IF(mesh_firmware_version_compare("2.7.26.54e0d8d", "2.7.26.ffffff0") != 0,
                      "two builds of the same three numbers are one release");
    MESH_TEST_FAIL_IF(mesh_firmware_version_compare("v2.7.26.54e0d8d", "2.7.26.54e0d8d") != 0,
                      "a leading v is not part of the version");
    MESH_TEST_FAIL_IF(mesh_firmware_version_compare("2.7", "2.7.1") >= 0,
                      "a missing component reads as zero");
    MESH_TEST_FAIL_IF(mesh_firmware_version_compare("", "2.7.26.54e0d8d") >= 0,
                      "a radio that has not said what it runs is never up to date");
    MESH_TEST_FAIL_IF(mesh_firmware_version_compare(NULL, NULL) != 0, "nothing equals nothing");
    record_success(test_name);
}

/*
 * The third document, and the two shapes it comes in.
 *
 * Both fixtures were pulled out of the real 2.7.26 release zips - the T114's from the
 * nrf52840 one, the Heltec V3's from the esp32s3 one - by the range read the client makes, and
 * they are not the same shape. The nRF52 manifest has no partition table and no `part_name` on
 * any file; the ESP32 one has six partitions and names three. A parser written against either
 * alone is a parser that finds nothing for half the boards this feature serves.
 */
MESH_TEST_CASE(firmware_catalog_reads_an_nrf52_manifest, unit) {
    size_t len = 0U;
    char *const document = mesh_test_data_read("t114_2.7.26.mt.json", &len);
    MESH_TEST_FAIL_IF(document == NULL, "tests/data/t114_2.7.26.mt.json should be readable");

    struct mesh_firmware_manifest manifest;
    const bool parsed = mesh_firmware_manifest_parse(document, len, &manifest);
    free(document);
    MESH_TEST_FAIL_IF(!parsed, "the T114's manifest should parse");
    MESH_TEST_FAIL_IF(strcmp(manifest.target, "heltec-mesh-node-t114") != 0,
                      "it names the build target we asked the zip for");
    MESH_TEST_FAIL_IF(manifest.hw_model != 69U, "and the model the radio reports");
    MESH_TEST_FAIL_IF(strcmp(manifest.version, "2.7.26.54e0d8d") != 0,
                      "and the release it came out of");
    MESH_TEST_FAIL_IF(!manifest.requires_dfu, "a T114 is flashed through its bootloader");
    /*
     * The two spellings that agree here and do not on an S3 - which is what makes this pair of
     * cases worth having rather than one.
     */
    MESH_TEST_FAIL_IF(strcmp(manifest.mcu, "nrf52840") != 0 ||
                          strcmp(manifest.architecture, "nrf52840") != 0,
                      "nRF52840 spells its mcu and its architecture the same way");
    MESH_TEST_FAIL_IF(manifest.count != 4U || manifest.found != 4U,
                      "the T114 publishes four files");

    const struct mesh_firmware_image *const image =
        mesh_firmware_manifest_image(&manifest, MESH_FIRMWARE_PATH_USB);
    MESH_TEST_FAIL_IF(image == NULL, "and one of them is the UF2 the USB path writes");
    MESH_TEST_FAIL_IF(strcmp(image->name, "firmware-heltec-mesh-node-t114-2.7.26.54e0d8d.uf2") != 0,
                      "which is the basename the zip is searched for");
    MESH_TEST_FAIL_IF(image->bytes != 1467392ULL,
                      "and its length is what the inflate must produce");
    MESH_TEST_FAIL_IF(strcmp(image->md5, "ddc727065b5e2a311fad8e3523fc5327") != 0,
                      "the hash is carried, md5 and all, even though we cannot check it");
    MESH_TEST_FAIL_IF(image->part[0] != '\0',
                      "and no file here carries a part_name, which is the whole point");

    /* The refusal that matters: asking an nRF52 manifest for an app0 partition finds nothing,
       rather than falling back to something that looks close. */
    MESH_TEST_FAIL_IF(mesh_firmware_manifest_image(&manifest, MESH_FIRMWARE_PATH_BLE) != NULL,
                      "there is no BLE image in an nRF52 manifest");
    MESH_TEST_FAIL_IF(mesh_firmware_manifest_image(&manifest, MESH_FIRMWARE_PATH_NONE) != NULL,
                      "and no image at all when there is no path");
    record_success(test_name);
}

/*
 * The ESP32-S3 shape, where the selector is a partition name and three files are wrong answers.
 *
 * `firmware-….bin` is app0 and is the one. `firmware-….factory.bin` is the whole flash
 * including a fresh filesystem - flashing it over BLE would wipe the radio's settings.
 * `littlefs-….bin` is that filesystem. And `mt-esp32s3-ota.bin` is the OTA loader itself,
 * declared app1: writing it is how the loader gets updated and is not something this client
 * will ever do, but it ships in the same zip and is a `.bin` like the other three.
 */
MESH_TEST_CASE(firmware_catalog_reads_an_esp32_manifest, unit) {
    size_t len = 0U;
    char *const document = mesh_test_data_read("heltec_v3_2.7.26.mt.json", &len);
    MESH_TEST_FAIL_IF(document == NULL, "tests/data/heltec_v3_2.7.26.mt.json should be readable");

    struct mesh_firmware_manifest manifest;
    const bool parsed = mesh_firmware_manifest_parse(document, len, &manifest);
    free(document);
    MESH_TEST_FAIL_IF(!parsed, "the Heltec V3's manifest should parse");
    MESH_TEST_FAIL_IF(manifest.hw_model != 43U, "the V3 is model 43");
    MESH_TEST_FAIL_IF(manifest.requires_dfu, "and it is not flashed through a bootloader");
    /*
     * The three-spellings trap, caught: `mcu` is what the release manifest's `platform` says
     * and is what goes in a URL; `architecture` is what deviceHardware says and is what the
     * path lookup keys on. They differ by a hyphen on exactly this family.
     */
    MESH_TEST_FAIL_IF(strcmp(manifest.mcu, "esp32s3") != 0,
                      "the V3's mcu is esp32s3, with no hyphen");
    MESH_TEST_FAIL_IF(strcmp(manifest.architecture, "esp32-s3") != 0,
                      "and its architecture is esp32-s3, with one");
    MESH_TEST_FAIL_IF(manifest.count != 5U, "the V3 publishes five files");

    const struct mesh_firmware_image *const image =
        mesh_firmware_manifest_image(&manifest, MESH_FIRMWARE_PATH_BLE);
    MESH_TEST_FAIL_IF(image == NULL, "one of which is app0");
    MESH_TEST_FAIL_IF(strcmp(image->name, "firmware-heltec-v3-2.7.26.54e0d8d.bin") != 0,
                      "and it is the plain .bin, not the factory one");
    MESH_TEST_FAIL_IF(image->bytes != 2109248ULL, "2,109,248 bytes of it");
    MESH_TEST_FAIL_IF(strcmp(image->part, "app0") != 0, "declared app0");

    /* And nothing here is a UF2, so the USB path has no answer rather than a near one. */
    MESH_TEST_FAIL_IF(mesh_firmware_manifest_image(&manifest, MESH_FIRMWARE_PATH_USB) != NULL,
                      "an ESP32 manifest has no UF2 in it");
    record_success(test_name);
}

/* A manifest that is not one, and one that lists nothing. Both parse or refuse deliberately:
   "the document did not parse" and "the document held no files" are different rows. */
MESH_TEST_CASE(firmware_catalog_manifest_survives_a_shape_it_did_not_expect, unit) {
    struct mesh_firmware_manifest manifest;
    MESH_TEST_FAIL_IF(mesh_firmware_manifest_parse("[]", 0U, &manifest),
                      "an array is not a manifest");
    MESH_TEST_FAIL_IF(mesh_firmware_manifest_parse("{\"files\":", 0U, &manifest),
                      "and neither is a truncated one");
    MESH_TEST_FAIL_IF(!mesh_firmware_manifest_parse("{}", 0U, &manifest), "an empty object parses");
    MESH_TEST_FAIL_IF(manifest.count != 0U || manifest.files[0].name[0] != '\0',
                      "and holds nothing");
    MESH_TEST_FAIL_IF(mesh_firmware_manifest_image(&manifest, MESH_FIRMWARE_PATH_USB) != NULL,
                      "so it has no image either");
    MESH_TEST_FAIL_IF(mesh_firmware_manifest_image(NULL, MESH_FIRMWARE_PATH_USB) != NULL,
                      "and neither does no manifest at all");
    /* A file with no name is not a file we could ask the zip for, so it is not counted. */
    MESH_TEST_FAIL_IF(!mesh_firmware_manifest_parse(
                          "{\"files\":[{\"md5\":\"x\"},{\"name\":\"a.uf2\"}]}", 0U, &manifest),
                      "a nameless entry does not stop the walk");
    MESH_TEST_FAIL_IF(manifest.count != 1U, "but it is not one of the files");
    record_success(test_name);
}
