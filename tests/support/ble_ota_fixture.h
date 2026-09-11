#ifndef MESH_TEST_BLE_OTA_FIXTURE_H
#define MESH_TEST_BLE_OTA_FIXTURE_H

#include "mesh/utils/sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A fake ESP32 OTA loader behind the bluez mock's write hook.
 *
 * It answers the way `meshtastic/esp32-unified-ota`'s OtaProcessor does - VERSION, then OTA
 * with ERASING and OK, then an ACK per write and OK for the last - and it really hashes what it
 * is sent and compares it with the hash the OTA command named, so a test that passes has sent
 * the bytes it announced. Answers are queued rather than emitted from inside the write, because
 * a real one arrives on a later turn of the loop: mesh_test_ota_loader_flush() delivers them,
 * and a test calls it between ticks.
 */

#define MESH_TEST_OTA_LOADER_QUEUE 16U
#define MESH_TEST_OTA_LOADER_TEXT 96U
#define MESH_TEST_OTA_LOADER_COMMANDS 4U

struct mesh_test_ota_loader {
    char notify_path[192];

    /* What it does. Survives mesh_test_ota_loader_restart(), like a loader's own code does. */
    bool silent;            /* answers nothing: the old loader, or a link that is not up */
    bool refuse_start;      /* ERR Hash Rejected (NVS Mismatch): it holds another image's hash */
    bool corrupt;           /* hashes one byte more than it was sent: ERR Hash Mismatch */
    bool ok_instead_of_ack; /* answers a chunk with OK: the cadence lost */
    size_t ack_limit;       /* stops answering after this many ACKs; 0 never stops */

    /* What it saw. */
    bool downloading;
    size_t expected_size;
    char expected_hex[MESH_SHA256_HEX_LEN];
    struct mesh_sha256 hasher;
    size_t received;
    size_t acks;
    size_t writes;
    size_t largest_write;
    size_t first_binary_len;
    size_t uneven_writes; /* chunks other than the last that were not the first one's length */
    bool finished_ok;
    char commands[MESH_TEST_OTA_LOADER_COMMANDS][128];
    size_t command_count;

    char queue[MESH_TEST_OTA_LOADER_QUEUE][MESH_TEST_OTA_LOADER_TEXT];
    size_t queued;
};

/* The first 48 bytes of `firmware-heltec-v3-2.7.26.54e0d8d.bin` as the release published them:
   magic 0xE9, seven segments, chip id 0x0009 at offset 12, the app descriptor's magic at 32. */
extern const uint8_t mesh_test_esp_header[48];

/* Everything zeroed; answers go to `device_path`'s notify characteristic as the mock names it. */
void mesh_test_ota_loader_init(struct mesh_test_ota_loader *loader, const char *device_path);

/* What a real loader does when the link drops: forgets the transfer, keeps its behaviour. */
void mesh_test_ota_loader_restart(struct mesh_test_ota_loader *loader);

/* The mock's write_hook. `userdata` is the loader. */
void mesh_test_ota_loader_write(void *userdata, const char *char_path, const uint8_t *data,
                                size_t len);

/* Emits everything queued as notifications, in order. */
void mesh_test_ota_loader_flush(struct mesh_test_ota_loader *loader);

/* A `len`-byte ESP32 application image for `chip`: the real header above with the chip id
   rewritten, and a counting pattern behind it. The caller frees it. */
uint8_t *mesh_test_esp_image(size_t len, uint16_t chip);

#endif /* MESH_TEST_BLE_OTA_FIXTURE_H */
