#include "support/ble_ota_fixture.h"

#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/ble_ota.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const uint8_t mesh_test_esp_header[48] = {
    0xE9, 0x07, 0x02, 0x3F, 0xD8, 0x72, 0x37, 0x40, 0xEE, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x00, 0x18, 0x3C, 0x30, 0x37, 0x07, 0x00,
    0x32, 0x54, 0xCD, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

void mesh_test_ota_loader_init(struct mesh_test_ota_loader *loader, const char *device_path) {
    memset(loader, 0, sizeof *loader);
    snprintf(loader->notify_path, sizeof loader->notify_path, "%s/%s", device_path,
             MESH_BLE_OTA_NOTIFY_UUID);
}

void mesh_test_ota_loader_restart(struct mesh_test_ota_loader *loader) {
    loader->downloading = false;
    loader->expected_size = 0U;
    loader->expected_hex[0] = '\0';
    loader->received = 0U;
    loader->acks = 0U;
    loader->first_binary_len = 0U;
    loader->uneven_writes = 0U;
    loader->finished_ok = false;
    loader->queued = 0U;
}

static void loader_say(struct mesh_test_ota_loader *loader, const char *text) {
    if (loader->queued < MESH_TEST_OTA_LOADER_QUEUE) {
        mesh_str_copy(loader->queue[loader->queued++], MESH_TEST_OTA_LOADER_TEXT, text);
    }
}

static void loader_command(struct mesh_test_ota_loader *loader, const uint8_t *data, size_t len) {
    char text[160];
    const size_t copy = len < sizeof text - 1U ? len : sizeof text - 1U;
    memcpy(text, data, copy);
    text[copy] = '\0';
    if (loader->command_count < MESH_TEST_OTA_LOADER_COMMANDS) {
        mesh_str_copy(loader->commands[loader->command_count++], sizeof loader->commands[0], text);
    }
    char *const newline = strchr(text, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }

    if (strcmp(text, "VERSION") == 0) {
        loader_say(loader, "OK 43 2.7.26.54e0d8d 7 v1.0.0\n");
        return;
    }
    if (strncmp(text, "OTA", 3U) == 0) {
        unsigned size = 0U;
        char hex[MESH_SHA256_HEX_LEN] = {0};
        if (sscanf(text + 3, "%u %64s", &size, hex) != 2) {
            loader_say(loader, "ERR Invalid Format\n");
            return;
        }
        if (loader->refuse_start) {
            loader_say(loader, "ERR Hash Rejected (NVS Mismatch)\n");
            return;
        }
        loader->expected_size = size;
        mesh_str_copy(loader->expected_hex, sizeof loader->expected_hex, hex);
        loader->received = 0U;
        mesh_sha256_init(&loader->hasher);
        loader->downloading = true;
        loader_say(loader, "ERASING\n");
        loader_say(loader, "OK\n");
        return;
    }
    loader_say(loader, "ERR Unknown Command\n");
}

void mesh_test_ota_loader_write(void *userdata, const char *char_path, const uint8_t *data,
                                size_t len) {
    (void)char_path;
    struct mesh_test_ota_loader *const loader = (struct mesh_test_ota_loader *)userdata;
    loader->writes += 1U;
    if (len > loader->largest_write) {
        loader->largest_write = len;
    }
    if (loader->silent) {
        return;
    }
    if (!loader->downloading) {
        loader_command(loader, data, len);
        return;
    }

    mesh_sha256_update(&loader->hasher, data, len);
    if (loader->corrupt && loader->received == 0U) {
        const uint8_t extra = 0xFFU;
        mesh_sha256_update(&loader->hasher, &extra, 1U);
    }
    if (loader->first_binary_len == 0U) {
        loader->first_binary_len = len;
    }
    loader->received += len;

    if (loader->received >= loader->expected_size) {
        loader->downloading = false;
        if (loader->received > loader->expected_size) {
            loader_say(loader, "ERR Size Mismatch\n");
            return;
        }
        uint8_t digest[MESH_SHA256_DIGEST_LEN];
        char hex[MESH_SHA256_HEX_LEN];
        mesh_sha256_final(&loader->hasher, digest);
        mesh_sha256_hex(digest, hex, sizeof hex);
        if (strcasecmp(hex, loader->expected_hex) == 0) {
            loader->finished_ok = true;
            loader_say(loader, "OK\n");
        } else {
            loader_say(loader, "ERR Hash Mismatch\n");
        }
        return;
    }

    if (len != loader->first_binary_len) {
        loader->uneven_writes += 1U;
    }
    if (loader->ok_instead_of_ack) {
        loader_say(loader, "OK\n");
        return;
    }
    if (loader->ack_limit != 0U && loader->acks >= loader->ack_limit) {
        return;
    }
    loader->acks += 1U;
    loader_say(loader, "ACK\n");
}

void mesh_test_ota_loader_flush(struct mesh_test_ota_loader *loader) {
    const size_t queued = loader->queued;
    loader->queued = 0U;
    for (size_t i = 0; i < queued; ++i) {
        mesh_bluez_client_mock_emit_notification(loader->notify_path,
                                                 (const uint8_t *)loader->queue[i],
                                                 strlen(loader->queue[i]));
    }
}

uint8_t *mesh_test_esp_image(size_t len, uint16_t chip) {
    if (len < sizeof mesh_test_esp_header) {
        return NULL;
    }
    uint8_t *const image = malloc(len);
    if (image == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < len; ++i) {
        image[i] = (uint8_t)(i * 7U + 3U);
    }
    memcpy(image, mesh_test_esp_header, sizeof mesh_test_esp_header);
    image[12] = (uint8_t)(chip & 0xFFU);
    image[13] = (uint8_t)(chip >> 8);
    return image;
}
