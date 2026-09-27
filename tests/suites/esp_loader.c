#define _POSIX_C_SOURCE 200809L

/*
 * The conversation with an ESP32's ROM bootloader, against a fake ROM on the far end of a
 * socketpair.
 *
 * The fake speaks the protocol as the Heltec V3 did when it was measured (see
 * inkwell/codec/esp_rom.h): four status bytes on every answer, MD5 in hex text, a 1 KB block
 * and nothing larger. What it cannot be is a chip - the control lines go to inkwell's mock,
 * which records them, and "the reset worked" is the fake starting to answer SYNC.
 */

#include "framework/mesh_test.h"

#include "mesh/core/firmware_serial.h"
#include "mesh/transport/esp_loader.h"

#include "inkwell/codec/esp_image.h"
#include "inkwell/codec/md5.h"
#include "inkwell/io/serial.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define FAKE_FLASH (128U * 1024U)

struct fake_rom {
    int fd;
    uint8_t flash[FAKE_FLASH];
    uint32_t magic;
    /* SYNCs ignored before one is answered; UINT32_MAX never answers. */
    uint32_t deaf_syncs;
    uint32_t syncs;
    /* Refuse this op with error 0x05, once. 0 refuses nothing. */
    uint8_t refuse_op;
    /* Flip a byte of what was written, so the MD5 the ROM reports is not the sent one. */
    bool corrupt;
    uint32_t begin_offset;
    size_t begins;
    size_t blocks;
    uint8_t in[4096];
    size_t in_len;
};

static void fake_answer(struct fake_rom *rom, uint8_t op, uint32_t value, const uint8_t *data,
                        size_t len, uint8_t status, uint8_t error) {
    uint8_t raw[128];
    const size_t size = len + 4U;
    raw[0] = 0x01U;
    raw[1] = op;
    raw[2] = (uint8_t)size;
    raw[3] = (uint8_t)(size >> 8);
    memcpy(raw + 4, &value, 4U);
    if (len > 0U) {
        memcpy(raw + 8, data, len);
    }
    raw[8U + len] = status;
    raw[9U + len] = error;
    raw[10U + len] = 0U;
    raw[11U + len] = 0U;
    uint8_t out[300];
    size_t at = 0U;
    out[at++] = 0xC0U;
    for (size_t i = 0; i < 8U + size; ++i) {
        if (raw[i] == 0xC0U || raw[i] == 0xDBU) {
            out[at++] = 0xDBU;
            out[at++] = raw[i] == 0xC0U ? 0xDCU : 0xDDU;
        } else {
            out[at++] = raw[i];
        }
    }
    out[at++] = 0xC0U;
    (void)write(rom->fd, out, at);
}

static uint32_t word(const uint8_t *at) {
    uint32_t value;
    memcpy(&value, at, sizeof value);
    return value;
}

static void fake_request(struct fake_rom *rom, const uint8_t *req, size_t len) {
    if (len < 8U || req[0] != 0x00U) {
        return;
    }
    const uint8_t op = req[1];
    const uint8_t *body = req + 8;
    if (rom->refuse_op == op) {
        rom->refuse_op = 0U;
        fake_answer(rom, op, 0U, NULL, 0U, 1U, 0x05U);
        return;
    }
    switch (op) {
    case INKWELL_ESP_ROM_SYNC:
        rom->syncs += 1U;
        if (rom->syncs > rom->deaf_syncs) {
            /* The ROM answers SYNC eight times over; the loader must take the first and
               ignore the rest. */
            for (int i = 0; i < 8; ++i) {
                fake_answer(rom, op, 0x20120707U, NULL, 0U, 0U, 0U);
            }
        }
        return;
    case INKWELL_ESP_ROM_READ_REG:
        fake_answer(rom, op, rom->magic, NULL, 0U, 0U, 0U);
        return;
    case INKWELL_ESP_ROM_FLASH_BEGIN: {
        const uint32_t size = word(body);
        rom->begin_offset = word(body + 12);
        rom->begins += 1U;
        if (rom->begin_offset + size <= FAKE_FLASH) {
            memset(rom->flash + rom->begin_offset, 0xFF, size);
        }
        fake_answer(rom, op, 0U, NULL, 0U, 0U, 0U);
        return;
    }
    case INKWELL_ESP_ROM_FLASH_DATA: {
        const uint32_t size = word(body);
        const uint32_t seq = word(body + 4);
        const uint32_t at = rom->begin_offset + seq * INKWELL_ESP_ROM_BLOCK;
        if (at + size <= FAKE_FLASH) {
            memcpy(rom->flash + at, body + 16, size);
            if (rom->corrupt && seq == 0U) {
                rom->flash[at] ^= 0x01U;
            }
        }
        rom->blocks += 1U;
        fake_answer(rom, op, 0U, NULL, 0U, 0U, 0U);
        return;
    }
    case INKWELL_ESP_ROM_SPI_FLASH_MD5: {
        const uint32_t offset = word(body);
        const uint32_t size = word(body + 4);
        uint8_t digest[INKWELL_MD5_DIGEST_LEN];
        char hex[INKWELL_MD5_HEX_LEN];
        struct inkwell_md5 md5;
        inkwell_md5_init(&md5);
        inkwell_md5_update(&md5, rom->flash + offset, size);
        inkwell_md5_final(&md5, digest);
        inkwell_md5_hex(digest, hex, sizeof hex);
        fake_answer(rom, op, 0U, (const uint8_t *)hex, 32U, 0U, 0U);
        return;
    }
    default:
        fake_answer(rom, op, 0U, NULL, 0U, 0U, 0U);
        return;
    }
}

/* Reads what the loader wrote and answers every whole request in it. */
static void fake_service(struct fake_rom *rom) {
    for (;;) {
        const ssize_t got = read(rom->fd, rom->in + rom->in_len, sizeof rom->in - rom->in_len);
        if (got <= 0) {
            break;
        }
        rom->in_len += (size_t)got;
    }
    for (;;) {
        uint8_t *const start = memchr(rom->in, 0xC0, rom->in_len);
        if (start == NULL) {
            rom->in_len = 0U;
            return;
        }
        const size_t from = (size_t)(start - rom->in);
        uint8_t *const end = memchr(start + 1, 0xC0, rom->in_len - from - 1U);
        if (end == NULL) {
            memmove(rom->in, start, rom->in_len - from);
            rom->in_len -= from;
            return;
        }
        uint8_t req[INKWELL_ESP_ROM_REQUEST_MAX];
        size_t len = 0U;
        for (uint8_t *p = start + 1; p < end; ++p) {
            if (*p == 0xDBU && p + 1 < end) {
                req[len++] = p[1] == 0xDCU ? 0xC0U : 0xDBU;
                ++p;
            } else {
                req[len++] = *p;
            }
        }
        fake_request(rom, req, len);
        const size_t consumed = (size_t)(end - rom->in) + 1U;
        memmove(rom->in, rom->in + consumed, rom->in_len - consumed);
        rom->in_len -= consumed;
    }
}

/* Runs the conversation on a fake clock, 10 ms a turn, until it ends or a minute passes. */
static void run(struct mesh_esp_loader *loader, struct fake_rom *rom, uint64_t *now) {
    for (int turn = 0; turn < 6000 && mesh_esp_loader_busy(loader); ++turn) {
        *now += 10U;
        mesh_esp_loader_tick(loader, *now);
        fake_service(rom);
        mesh_esp_loader_pump(loader, *now);
        fake_service(rom);
    }
}

static int open_pair(int pair[2], struct fake_rom *rom) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        return -1;
    }
    (void)fcntl(pair[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pair[1], F_SETFL, O_NONBLOCK);
    rom->fd = pair[0];
    struct inkwell_serial_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.open_fd = pair[1];
    inkwell_serial_mock_enable(&mock);
    return 0;
}

static void close_pair(int pair[2]) {
    inkwell_serial_mock_disable();
    close(pair[0]);
    close(pair[1]);
}

static struct fake_rom g_rom;
static uint8_t g_image[5000];
static uint8_t g_small[300];

static void fill(void) {
    memset(&g_rom, 0, sizeof g_rom);
    g_rom.magic = 0x00000009U; /* an ESP32-S3 */
    for (size_t i = 0; i < sizeof g_image; ++i) {
        /* Every byte SLIP has to escape, many times over. */
        g_image[i] = (uint8_t)(i % 7U == 0U ? 0xC0U : (i % 11U == 0U ? 0xDBU : i * 13U));
    }
    memset(g_small, 0xFF, sizeof g_small);
}

MESH_TEST_CASE(esp_loader_writes_and_verifies_every_region, unit) {
    fill();
    int pair[2];
    MESH_TEST_FAIL_IF(open_pair(pair, &g_rom) != 0, "no socketpair");
    g_rom.deaf_syncs = 4U; /* the ROM takes a few SYNCs to tune its baud detection */
    const struct mesh_esp_loader_region regions[2] = {
        {0x4000U, g_image, sizeof g_image},
        {0x2000U, g_small, sizeof g_small},
    };
    static struct mesh_esp_loader loader;
    uint64_t now = 1000U;
    const int started = mesh_esp_loader_start(&loader, NULL, "/dev/ttyUSB0",
                                              INKWELL_ESP_CHIP_ESP32_S3, regions, 2U, 0U, now);
    run(&loader, &g_rom, &now);
    bool dtr = true;
    bool rts = true;
    const size_t lines = inkwell_serial_mock_lines_calls(&dtr, &rts);
    const unsigned baud = inkwell_serial_mock_baud();
    close_pair(pair);

    MESH_TEST_FAIL_IF(started != 0, "the conversation should start");
    MESH_TEST_FAIL_IF(loader.state != MESH_ESP_LOADER_DONE,
                      mesh_esp_loader_error_name(loader.error));
    MESH_TEST_FAIL_IF(memcmp(g_rom.flash + 0x4000U, g_image, sizeof g_image) != 0 ||
                          memcmp(g_rom.flash + 0x2000U, g_small, sizeof g_small) != 0,
                      "the flash holds exactly what was sent, escapes and all");
    MESH_TEST_FAIL_IF(g_rom.begins != 2U || g_rom.blocks != 5U + 1U,
                      "one erase per region, one block per started kilobyte");
    MESH_TEST_FAIL_IF(loader.chip != INKWELL_ESP_CHIP_ESP32_S3, "the chip it found");
    /* Into the ROM is three changes, and back out is two more. */
    MESH_TEST_FAIL_IF(lines != 5U || dtr || rts,
                      "reset in, reset out, and both lines left released");
    MESH_TEST_FAIL_IF(baud != MESH_ESP_LOADER_WRITE_BAUD, "the write runs at the faster rate");
    MESH_TEST_FAIL_IF(mesh_esp_loader_progress(&loader) != 100U, "and all of it counted");
    record_success(test_name);
}

MESH_TEST_CASE(esp_loader_refuses_another_chip_before_touching_flash, unit) {
    fill();
    int pair[2];
    MESH_TEST_FAIL_IF(open_pair(pair, &g_rom) != 0, "no socketpair");
    g_rom.magic = 0x00F01D83U; /* the original ESP32 */
    const struct mesh_esp_loader_region region = {0x4000U, g_image, sizeof g_image};
    static struct mesh_esp_loader loader;
    uint64_t now = 1000U;
    (void)mesh_esp_loader_start(&loader, NULL, "/dev/ttyUSB0", INKWELL_ESP_CHIP_ESP32_S3, &region,
                                1U, 0U, now);
    run(&loader, &g_rom, &now);
    bool dtr = true;
    bool rts = true;
    const size_t lines = inkwell_serial_mock_lines_calls(&dtr, &rts);
    close_pair(pair);
    MESH_TEST_FAIL_IF(loader.state != MESH_ESP_LOADER_FAILED ||
                          loader.error != MESH_ESP_LOADER_ERROR_WRONG_CHIP,
                      "an S3 image is not for an ESP32");
    MESH_TEST_FAIL_IF(g_rom.begins != 0U, "and nothing was erased to find that out");
    MESH_TEST_FAIL_IF(lines != 5U || dtr || rts,
                      "the chip is reset out of its ROM, not left in download mode");
    record_success(test_name);
}

MESH_TEST_CASE(esp_loader_gives_up_on_a_silent_chip, unit) {
    fill();
    int pair[2];
    MESH_TEST_FAIL_IF(open_pair(pair, &g_rom) != 0, "no socketpair");
    g_rom.deaf_syncs = UINT32_MAX;
    const struct mesh_esp_loader_region region = {0x4000U, g_image, sizeof g_image};
    static struct mesh_esp_loader loader;
    uint64_t now = 1000U;
    (void)mesh_esp_loader_start(&loader, NULL, "/dev/ttyUSB0", MESH_ESP_LOADER_ANY_CHIP, &region,
                                1U, 0U, now);
    run(&loader, &g_rom, &now);
    const size_t lines = inkwell_serial_mock_lines_calls(NULL, NULL);
    close_pair(pair);
    MESH_TEST_FAIL_IF(loader.error != MESH_ESP_LOADER_ERROR_SILENT, "nothing answered SYNC");
    MESH_TEST_FAIL_IF(loader.attempts != 3U || lines != 11U || g_rom.syncs != 30U,
                      "three resets of three line changes, ten SYNCs after each, and one out");
    record_success(test_name);
}

MESH_TEST_CASE(esp_loader_reports_a_refusal_and_a_bad_digest, unit) {
    fill();
    int pair[2];
    MESH_TEST_FAIL_IF(open_pair(pair, &g_rom) != 0, "no socketpair");
    g_rom.refuse_op = INKWELL_ESP_ROM_FLASH_BEGIN;
    const struct mesh_esp_loader_region region = {0x4000U, g_image, sizeof g_image};
    static struct mesh_esp_loader loader;
    uint64_t now = 1000U;
    (void)mesh_esp_loader_start(&loader, NULL, "/dev/ttyUSB0", MESH_ESP_LOADER_ANY_CHIP, &region,
                                1U, 0U, now);
    run(&loader, &g_rom, &now);
    MESH_TEST_FAIL_IF_CLEANUP(loader.error != MESH_ESP_LOADER_ERROR_REFUSED ||
                                  loader.rom_error != 0x05U,
                              close_pair(pair), "the ROM's own refusal, with its number");
    MESH_TEST_FAIL_IF_CLEANUP(!loader.erase_sent, close_pair(pair),
                              "an erase asked for is an erase that may have happened");

    g_rom.corrupt = true;
    (void)mesh_esp_loader_start(&loader, NULL, "/dev/ttyUSB0", MESH_ESP_LOADER_ANY_CHIP, &region,
                                1U, 0U, now);
    run(&loader, &g_rom, &now);
    close_pair(pair);
    MESH_TEST_FAIL_IF(loader.error != MESH_ESP_LOADER_ERROR_VERIFY,
                      "every block taken and the flash hashing to something else is a failure");
    record_success(test_name);
}

MESH_TEST_CASE(esp_loader_cancelled_in_the_rom_resets_the_chip_out, unit) {
    fill();
    int pair[2];
    MESH_TEST_FAIL_IF(open_pair(pair, &g_rom) != 0, "no socketpair");
    const struct mesh_esp_loader_region region = {0x4000U, g_image, sizeof g_image};
    static struct mesh_esp_loader loader;
    uint64_t now = 1000U;
    (void)mesh_esp_loader_start(&loader, NULL, "/dev/ttyUSB0", MESH_ESP_LOADER_ANY_CHIP, &region,
                                1U, 0U, now);
    for (int turn = 0; turn < 6000 && loader.state != MESH_ESP_LOADER_ATTACHING; ++turn) {
        now += 10U;
        mesh_esp_loader_tick(&loader, now);
        fake_service(&g_rom);
        mesh_esp_loader_pump(&loader, now);
        fake_service(&g_rom);
    }
    const bool in_rom = loader.state == MESH_ESP_LOADER_ATTACHING;
    mesh_esp_loader_cancel(&loader);
    bool dtr = true;
    bool rts = true;
    const size_t lines = inkwell_serial_mock_lines_calls(&dtr, &rts);
    close_pair(pair);
    MESH_TEST_FAIL_IF(!in_rom || loader.erase_sent, "stopped in the ROM, before any erase");
    MESH_TEST_FAIL_IF(lines != 5U || dtr || rts,
                      "reset in, then a reset out on the way, not a chip left in download mode");
    MESH_TEST_FAIL_IF(mesh_esp_loader_busy(&loader), "and the loader let go");
    record_success(test_name);
}

MESH_TEST_CASE(esp_loader_refuses_what_it_cannot_write, unit) {
    static struct mesh_esp_loader loader;
    const struct mesh_esp_loader_region empty = {0x4000U, g_image, 0U};
    MESH_TEST_FAIL_IF(
        mesh_esp_loader_start(&loader, NULL, "/dev/ttyUSB0", 0U, &empty, 1U, 0U, 0U) != -EINVAL,
        "an empty region");
    MESH_TEST_FAIL_IF(
        mesh_esp_loader_start(&loader, NULL, "/dev/ttyUSB0", 0U, &empty, 0U, 0U, 0U) != -EINVAL,
        "no regions");
    struct mesh_esp_loader zeroed;
    memset(&zeroed, 0, sizeof zeroed);
    mesh_esp_loader_cancel(&zeroed);
    MESH_TEST_FAIL_IF(mesh_esp_loader_busy(&zeroed), "a zeroed loader cancels to nothing");
    record_success(test_name);
}

/* ---- the handover over it ------------------------------------------------------------- */

/* The first 48 bytes of firmware-heltec-v3-2.7.26.54e0d8d.bin: an ESP32-S3 app header. */
static const uint8_t k_s3_header[48] = {
    0xE9, 0x07, 0x02, 0x3F, 0xD8, 0x72, 0x37, 0x40, 0xEE, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x00, 0x18, 0x3C, 0x30, 0x37, 0x07, 0x00,
    0x32, 0x54, 0xCD, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Stages `bytes` as a file and returns its path in `out`. */
static bool stage(const uint8_t *bytes, size_t len, char *out, size_t out_len) {
    const char *const dir = getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp";
    snprintf(out, out_len, "%s/esp_loader_XXXXXX", dir);
    const int fd = mkstemp(out);
    if (fd < 0) {
        return false;
    }
    const bool wrote = write(fd, bytes, len) == (ssize_t)len;
    close(fd);
    return wrote;
}

MESH_TEST_CASE(firmware_serial_writes_the_app_and_blanks_otadata, unit) {
    fill();
    memcpy(g_image, k_s3_header, sizeof k_s3_header);
    char path[256];
    MESH_TEST_FAIL_IF(!stage(g_image, sizeof g_image, path, sizeof path), "no staging file");
    int pair[2];
    MESH_TEST_FAIL_IF_CLEANUP(open_pair(pair, &g_rom) != 0, unlink(path), "no socketpair");
    /* The flash as a radio last updated over BLE leaves it: otadata pointing at app1. */
    memset(g_rom.flash, 0x00, sizeof g_rom.flash);
    struct inkwell_serial_port_info port;
    memset(&port, 0, sizeof port);
    snprintf(port.id, sizeof port.id, "1-1:1.0");
    snprintf(port.path, sizeof port.path, "/dev/ttyUSB0");
    port.kind = INKWELL_SERIAL_BRIDGE;
    port.bound = true;
    struct inkwell_serial_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.ports = &port;
    mock.port_count = 1U;
    mock.open_fd = pair[1];
    inkwell_serial_mock_enable(&mock);

    static struct mesh_firmware_serial serial;
    uint64_t now = 1000U;
    const int started =
        mesh_firmware_serial_start(&serial, NULL, path, "1-1:1.0", INKWELL_ESP_CHIP_ESP32_S3, now);
    for (int turn = 0; turn < 6000 && mesh_firmware_serial_busy(&serial); ++turn) {
        now += 10U;
        mesh_firmware_serial_tick(&serial, now);
        fake_service(&g_rom);
    }
    close_pair(pair);
    unlink(path);

    MESH_TEST_FAIL_IF(started != 0, serial.reason);
    MESH_TEST_FAIL_IF(serial.state != MESH_FIRMWARE_SERIAL_DONE, serial.reason);
    MESH_TEST_FAIL_IF(
        memcmp(g_rom.flash + MESH_FIRMWARE_SERIAL_APP_OFFSET, g_image, sizeof g_image) != 0,
        "the app went to app0");
    bool blank = true;
    for (size_t i = 0; i < MESH_FIRMWARE_SERIAL_OTADATA_LEN; ++i) {
        blank = blank && g_rom.flash[MESH_FIRMWARE_SERIAL_OTADATA_OFFSET + i] == 0xFFU;
    }
    MESH_TEST_FAIL_IF(!blank, "and otadata was erased, so the bootloader takes app0");
    MESH_TEST_FAIL_IF(g_rom.flash[0x9000] != 0x00U, "while nvs, the settings, was not touched");
    MESH_TEST_FAIL_IF(mesh_firmware_serial_radio_in_loader(&serial) || serial.image != NULL,
                      "a finished write strands nothing and holds no image");
    record_success(test_name);
}

/* One partition table row. */
static void table_row(uint8_t *at, uint8_t type, uint8_t subtype, uint32_t offset, uint32_t size,
                      const char *label) {
    memset(at, 0, 32U);
    at[0] = 0xAAU;
    at[1] = 0x50U;
    at[2] = type;
    at[3] = subtype;
    memcpy(at + 4, &offset, 4U);
    memcpy(at + 8, &size, 4U);
    memcpy(at + 12, label, strlen(label));
}

/*
 * A whole-flash image as merge_bin lays one out, shrunk to the fake's 128 KB: an S3 bootloader
 * at 0x0, the table at 0x8000, the app at 0x10000, 0xFF between. Two data partitions sit past
 * its end, where the last firmware's filesystem and core dump would be.
 */
static uint8_t g_whole[0x10000U + sizeof g_image];

static void fill_whole(void) {
    memset(g_whole, 0xFF, sizeof g_whole);
    memcpy(g_whole, k_s3_header, sizeof k_s3_header);
    memset(g_whole + 32, 0x00, 4U); /* a bootloader carries no app descriptor */
    uint8_t *const table = g_whole + 0x8000U;
    table_row(table + 0, 0x01U, 0x02U, 0x9000U, 0x5000U, "nvs");
    table_row(table + 32, 0x01U, 0x00U, 0xE000U, 0x2000U, "otadata");
    table_row(table + 64, 0x00U, 0x10U, 0x10000U, 0x8000U, "app0");
    table_row(table + 96, 0x01U, 0x82U, 0x18000U, 0x4000U, "spiffs");
    table_row(table + 128, 0x01U, 0x03U, 0x1C000U, 0x2000U, "coredump");
    memcpy(g_whole + 0x10000U, g_image, sizeof g_image);
    memcpy(g_whole + 0x10000U, k_s3_header, sizeof k_s3_header);
}

MESH_TEST_CASE(firmware_serial_writes_a_whole_flash_and_erases_what_the_last_one_kept, unit) {
    fill();
    fill_whole();
    char path[256];
    MESH_TEST_FAIL_IF(!stage(g_whole, sizeof g_whole, path, sizeof path), "no staging file");
    int pair[2];
    MESH_TEST_FAIL_IF_CLEANUP(open_pair(pair, &g_rom) != 0, unlink(path), "no socketpair");
    /* The last firmware's bytes everywhere. */
    memset(g_rom.flash, 0x5A, sizeof g_rom.flash);
    struct inkwell_serial_port_info port;
    memset(&port, 0, sizeof port);
    snprintf(port.id, sizeof port.id, "1-1:1.0");
    snprintf(port.path, sizeof port.path, "/dev/ttyUSB0");
    port.kind = INKWELL_SERIAL_BRIDGE;
    port.bound = true;
    struct inkwell_serial_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.ports = &port;
    mock.port_count = 1U;
    mock.open_fd = pair[1];
    inkwell_serial_mock_enable(&mock);

    static struct mesh_firmware_serial serial;
    uint64_t now = 1000U;
    const int started = mesh_firmware_serial_start_whole(&serial, NULL, path, "1-1:1.0",
                                                         INKWELL_ESP_CHIP_ESP32_S3, now);
    for (int turn = 0; turn < 20000 && mesh_firmware_serial_busy(&serial); ++turn) {
        now += 10U;
        mesh_firmware_serial_tick(&serial, now);
        fake_service(&g_rom);
    }
    close_pair(pair);
    unlink(path);

    MESH_TEST_FAIL_IF(started != 0, serial.reason);
    MESH_TEST_FAIL_IF(serial.state != MESH_FIRMWARE_SERIAL_DONE, serial.reason);
    MESH_TEST_FAIL_IF(memcmp(g_rom.flash, g_whole, sizeof g_whole) != 0,
                      "the image went down at 0x0, bootloader and table and all");
    bool blank = true;
    for (size_t i = 0x18000U; i < 0x1E000U; ++i) {
        blank = blank && g_rom.flash[i] == 0xFFU;
    }
    MESH_TEST_FAIL_IF(!blank, "the filesystem and core dump past it were erased");
    MESH_TEST_FAIL_IF(g_rom.flash[0x1E000U] != 0x5AU || g_rom.flash[0x12000U] != 0x5AU,
                      "and nothing the table does not call data was touched");
    MESH_TEST_FAIL_IF(g_rom.begins != 3U || g_rom.blocks != (sizeof g_whole + 1023U) / 1024U,
                      "one write and two erases that send no blocks");
    MESH_TEST_FAIL_IF(mesh_firmware_serial_progress(&serial) != 100U, "and all of it counted");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_serial_refuses_a_whole_image_that_is_not_one, unit) {
    fill();
    fill_whole();
    char path[256];
    static struct mesh_firmware_serial serial;
    inkwell_serial_mock_enable(NULL);

    /* An app image is not a whole flash: no bootloader, no table. */
    memcpy(g_image, k_s3_header, sizeof k_s3_header);
    MESH_TEST_FAIL_IF(!stage(g_image, sizeof g_image, path, sizeof path), "no staging file");
    int result =
        mesh_firmware_serial_start_whole(&serial, NULL, path, "", INKWELL_ESP_CHIP_ESP32_S3, 0U);
    unlink(path);
    MESH_TEST_FAIL_IF(result != -EINVAL || serial.error != MESH_FIRMWARE_SERIAL_ERROR_IMAGE,
                      "an app image is refused as a whole flash");

    /* The S3's whole image, for an original ESP32: its bootloader is at 0x1000. */
    MESH_TEST_FAIL_IF(!stage(g_whole, sizeof g_whole, path, sizeof path), "no staging file");
    result = mesh_firmware_serial_start_whole(&serial, NULL, path, "", INKWELL_ESP_CHIP_ESP32, 0U);
    MESH_TEST_FAIL_IF_CLEANUP(result != -EINVAL, unlink(path), "another chip's whole flash");
    unlink(path);

    /* An application where the bootloader goes, with the table and app as they should be. */
    memcpy(g_whole, k_s3_header, sizeof k_s3_header);
    MESH_TEST_FAIL_IF_CLEANUP(!stage(g_whole, sizeof g_whole, path, sizeof path), unlink(path),
                              "no staging file");
    result =
        mesh_firmware_serial_start_whole(&serial, NULL, path, "", INKWELL_ESP_CHIP_ESP32_S3, 0U);
    unlink(path);
    MESH_TEST_FAIL_IF_CLEANUP(result != -EINVAL, inkwell_serial_mock_disable(),
                              "an app in the bootloader's slot");
    memset(g_whole + 32, 0x00, 4U);

    /* A table with no app in it. */
    memset(g_whole + 0x8000U + 64U, 0xFF, 96U);
    MESH_TEST_FAIL_IF_CLEANUP(!stage(g_whole, sizeof g_whole, path, sizeof path), unlink(path),
                              "no staging file");
    result =
        mesh_firmware_serial_start_whole(&serial, NULL, path, "", INKWELL_ESP_CHIP_ESP32_S3, 0U);
    unlink(path);
    inkwell_serial_mock_disable();
    MESH_TEST_FAIL_IF(result != -EINVAL, "a table naming no application");
    MESH_TEST_FAIL_IF(g_rom.begins != 0U, "and not one of them reached a ROM");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_serial_refuses_before_the_radio_is_touched, unit) {
    fill();
    char path[256];
    static struct mesh_firmware_serial serial;

    /* Bytes that are not an app image for this chip. */
    MESH_TEST_FAIL_IF(!stage(g_image, sizeof g_image, path, sizeof path), "no staging file");
    inkwell_serial_mock_enable(NULL);
    int result = mesh_firmware_serial_start(&serial, NULL, path, "", INKWELL_ESP_CHIP_ESP32_S3, 0U);
    MESH_TEST_FAIL_IF_CLEANUP(result >= 0 || serial.error != MESH_FIRMWARE_SERIAL_ERROR_IMAGE,
                              unlink(path), "not an image");
    unlink(path);

    memcpy(g_image, k_s3_header, sizeof k_s3_header);
    MESH_TEST_FAIL_IF(!stage(g_image, sizeof g_image, path, sizeof path), "no staging file");
    result = mesh_firmware_serial_start(&serial, NULL, path, "", INKWELL_ESP_CHIP_ESP32, 0U);
    MESH_TEST_FAIL_IF_CLEANUP(result >= 0 || serial.error != MESH_FIRMWARE_SERIAL_ERROR_IMAGE,
                              unlink(path), "an S3 image for an ESP32");

    /* No port at all, and then only the chip's own USB. */
    result = mesh_firmware_serial_start(&serial, NULL, path, "", INKWELL_ESP_CHIP_ESP32_S3, 0U);
    MESH_TEST_FAIL_IF_CLEANUP(result >= 0 || serial.error != MESH_FIRMWARE_SERIAL_ERROR_NO_PORT,
                              unlink(path), "no port");
    struct inkwell_serial_port_info port;
    memset(&port, 0, sizeof port);
    snprintf(port.id, sizeof port.id, "1-1:1.0");
    snprintf(port.path, sizeof port.path, "/dev/ttyACM0");
    port.kind = INKWELL_SERIAL_NATIVE;
    port.vendor_id = 0x303AU;
    port.bound = true;
    struct inkwell_serial_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.ports = &port;
    mock.port_count = 1U;
    mock.open_fd = -1;
    inkwell_serial_mock_enable(&mock);
    result =
        mesh_firmware_serial_start(&serial, NULL, path, "1-1:1.0", INKWELL_ESP_CHIP_ESP32_S3, 0U);
    const size_t lines = inkwell_serial_mock_lines_calls(NULL, NULL);
    inkwell_serial_mock_disable();
    unlink(path);
    MESH_TEST_FAIL_IF(result >= 0 || serial.error != MESH_FIRMWARE_SERIAL_ERROR_NATIVE_USB,
                      "the chip's own USB is refused");
    MESH_TEST_FAIL_IF(lines != 0U || mesh_firmware_serial_radio_in_loader(&serial),
                      "and no refusal moved a line or left the radio anywhere");
    record_success(test_name);
}

/* Runs a handover against the fake until it ends. */
static void run_serial(struct mesh_firmware_serial *serial, uint64_t *now) {
    for (int turn = 0; turn < 6000 && mesh_firmware_serial_busy(serial); ++turn) {
        *now += 10U;
        mesh_firmware_serial_tick(serial, *now);
        fake_service(&g_rom);
    }
}

MESH_TEST_CASE(firmware_serial_falls_back_to_the_only_bridge_only_to_finish_a_write, unit) {
    fill();
    memcpy(g_image, k_s3_header, sizeof k_s3_header);
    char path[256];
    MESH_TEST_FAIL_IF(!stage(g_image, sizeof g_image, path, sizeof path), "no staging file");
    int pair[2];
    MESH_TEST_FAIL_IF_CLEANUP(open_pair(pair, &g_rom) != 0, unlink(path), "no socketpair");
    struct inkwell_serial_port_info port;
    memset(&port, 0, sizeof port);
    snprintf(port.id, sizeof port.id, "1-2:1.0");
    snprintf(port.path, sizeof port.path, "/dev/ttyUSB0");
    port.kind = INKWELL_SERIAL_BRIDGE;
    port.bound = true;
    struct inkwell_serial_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.ports = &port;
    mock.port_count = 1U;
    mock.open_fd = pair[1];
    inkwell_serial_mock_enable(&mock);
    static struct mesh_firmware_serial serial;
    memset(&serial, 0, sizeof serial);
    uint64_t now = 1000U;

    /* A fresh install naming a port that is not there is refused, not sent to another ESP32. */
    int result =
        mesh_firmware_serial_start(&serial, NULL, path, "1-1:1.0", INKWELL_ESP_CHIP_ESP32_S3, now);
    const bool strict = result < 0 && serial.error == MESH_FIRMWARE_SERIAL_ERROR_NO_PORT &&
                        !mesh_firmware_serial_radio_in_loader(&serial);

    /* A write that fails after its erase leaves the radio in its loader. */
    g_rom.refuse_op = INKWELL_ESP_ROM_FLASH_DATA;
    result =
        mesh_firmware_serial_start(&serial, NULL, path, "1-2:1.0", INKWELL_ESP_CHIP_ESP32_S3, now);
    run_serial(&serial, &now);
    const bool stranded = result == 0 && mesh_firmware_serial_radio_in_loader(&serial);

    /* A retry that fails before its own erase - no port at all - still says so. */
    mock.port_count = 0U;
    inkwell_serial_mock_enable(&mock);
    result =
        mesh_firmware_serial_start(&serial, NULL, path, "1-2:1.0", INKWELL_ESP_CHIP_ESP32_S3, now);
    const bool remembered = result < 0 && mesh_firmware_serial_radio_in_loader(&serial);

    /* And the cable back in another socket is another id, which the resume takes. */
    snprintf(port.id, sizeof port.id, "1-3:1.0");
    mock.port_count = 1U;
    inkwell_serial_mock_enable(&mock);
    result =
        mesh_firmware_serial_start(&serial, NULL, path, "1-2:1.0", INKWELL_ESP_CHIP_ESP32_S3, now);
    run_serial(&serial, &now);
    close_pair(pair);
    unlink(path);

    MESH_TEST_FAIL_IF(!strict, "a named port that is gone is refused on a fresh install");
    MESH_TEST_FAIL_IF(!stranded, "a refused block after the erase strands the radio");
    MESH_TEST_FAIL_IF(!remembered, "a retry that never reached the ROM keeps the banner");
    MESH_TEST_FAIL_IF(result != 0 || serial.state != MESH_FIRMWARE_SERIAL_DONE, serial.reason);
    MESH_TEST_FAIL_IF(mesh_firmware_serial_radio_in_loader(&serial) || serial.damaged,
                      "a finished write clears the damage");
    MESH_TEST_FAIL_IF(
        memcmp(g_rom.flash + MESH_FIRMWARE_SERIAL_APP_OFFSET, g_image, sizeof g_image) != 0,
        "the resume wrote the app");
    record_success(test_name);
}

MESH_TEST_CASE(firmware_serial_cancelled_after_the_erase_is_a_radio_in_its_loader, unit) {
    fill();
    memcpy(g_image, k_s3_header, sizeof k_s3_header);
    char path[256];
    MESH_TEST_FAIL_IF(!stage(g_image, sizeof g_image, path, sizeof path), "no staging file");
    int pair[2];
    MESH_TEST_FAIL_IF_CLEANUP(open_pair(pair, &g_rom) != 0, unlink(path), "no socketpair");
    struct inkwell_serial_port_info port;
    memset(&port, 0, sizeof port);
    snprintf(port.id, sizeof port.id, "1-1:1.0");
    snprintf(port.path, sizeof port.path, "/dev/ttyUSB0");
    port.kind = INKWELL_SERIAL_BRIDGE;
    port.bound = true;
    struct inkwell_serial_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.ports = &port;
    mock.port_count = 1U;
    mock.open_fd = pair[1];
    inkwell_serial_mock_enable(&mock);
    static struct mesh_firmware_serial serial;
    memset(&serial, 0, sizeof serial);
    uint64_t now = 1000U;

    const int started =
        mesh_firmware_serial_start(&serial, NULL, path, "1-1:1.0", INKWELL_ESP_CHIP_ESP32_S3, now);
    for (int turn = 0; turn < 6000 && serial.state != MESH_FIRMWARE_SERIAL_WRITING; ++turn) {
        now += 10U;
        mesh_firmware_serial_tick(&serial, now);
        fake_service(&g_rom);
    }
    const bool writing = serial.state == MESH_FIRMWARE_SERIAL_WRITING;
    mesh_firmware_serial_cancel(&serial);
    close_pair(pair);
    unlink(path);

    MESH_TEST_FAIL_IF(started != 0 || !writing, "the write should get under way");
    MESH_TEST_FAIL_IF(serial.state != MESH_FIRMWARE_SERIAL_FAILED ||
                          !mesh_firmware_serial_radio_in_loader(&serial),
                      "a cancel after the erase leaves a radio to recover, not an idle one");
    MESH_TEST_FAIL_IF(mesh_firmware_serial_busy(&serial), "and lets go of it");
    record_success(test_name);
}
