#define _POSIX_C_SOURCE 200809L

#include "mesh/transport/esp_loader.h"

#include "inkwell/base/fd.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"
#include "inkwell/codec/md5.h"
#include "inkwell/io/serial.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* esptool's classic reset: 100 ms held in reset, 50 ms with the strap low after EN rises. */
#define LOADER_RESET_HOLD_MS 100U
#define LOADER_STRAP_HOLD_MS 50U
/* SYNC is repeated this often, this many times, before the reset is tried again - and the
   reset this many times before the chip is taken to be silent. */
#define LOADER_SYNC_INTERVAL_MS 100U
#define LOADER_SYNC_TRIES 10U
#define LOADER_RESET_TRIES 3U
/* After CHANGE_BAUDRATE's answer, the ROM needs a moment at the new rate before it listens. */
#define LOADER_BAUD_SETTLE_MS 50U
/* An erase-only region counts toward progress as this fraction of its length: erasing is about
   an eighth of writing per byte, as a Heltec V3 measured it (1.5 MB in 6 s against 2.3 MB in
   80 s). Enough that the bar is not full while the erases still stand between the radio and a
   blank flash. */
#define LOADER_ERASE_PROGRESS_SHARE 8U

/* esptool's allowances: 3 s for anything, and per megabyte 30 s to erase and 8 s to hash. */
#define LOADER_COMMAND_TIMEOUT_MS 3000U
#define LOADER_ERASE_MS_PER_MB 30000U
#define LOADER_MD5_MS_PER_MB 8000U
#define LOADER_MIN_FLASH (4U * 1024U * 1024U)

static const char *const k_state_names[MESH_ESP_LOADER_STATE_COUNT] = {
    "idle",    "resetting", "syncing",   "identifying", "baud", "attaching",
    "erasing", "writing",   "verifying", "restarting",  "done", "failed",
};

static const char *const k_error_names[MESH_ESP_LOADER_ERROR_COUNT] = {
    "none", "port", "silent", "wrong chip", "refused", "timeout", "verify", "io",
};

const char *mesh_esp_loader_state_name(enum mesh_esp_loader_state state) {
    return state < MESH_ESP_LOADER_STATE_COUNT ? k_state_names[state] : "?";
}

const char *mesh_esp_loader_error_name(enum mesh_esp_loader_error error) {
    return error < MESH_ESP_LOADER_ERROR_COUNT ? k_error_names[error] : "?";
}

bool mesh_esp_loader_busy(const struct mesh_esp_loader *loader) {
    return loader != NULL && loader->state != MESH_ESP_LOADER_IDLE &&
           loader->state != MESH_ESP_LOADER_DONE && loader->state != MESH_ESP_LOADER_FAILED;
}

unsigned mesh_esp_loader_progress(const struct mesh_esp_loader *loader) {
    if (loader == NULL || loader->bytes_total == 0U) {
        return 0U;
    }
    if (loader->state == MESH_ESP_LOADER_DONE) {
        return 100U;
    }
    return (unsigned)((loader->bytes_written * 100U) / loader->bytes_total);
}

static uint64_t per_mb(size_t len, unsigned ms_per_mb) {
    const uint64_t scaled = ((uint64_t)len * ms_per_mb) / (1024U * 1024U);
    return scaled > LOADER_COMMAND_TIMEOUT_MS ? scaled : LOADER_COMMAND_TIMEOUT_MS;
}

static void loader_close(struct mesh_esp_loader *loader) {
    if (loader->watched && loader->loop != NULL) {
        (void)inkwell_loop_remove_fd(loader->loop, loader->fd);
    }
    loader->watched = false;
    if (loader->port_open) {
        inkwell_serial_close(loader->fd);
    }
    loader->port_open = false;
    loader->fd = -1;
}

static void loader_failed(struct mesh_esp_loader *loader, uint64_t now_ms) {
    loader->state = MESH_ESP_LOADER_FAILED;
    loader->resetting_out = false;
    loader->finished_ms = now_ms;
    loader_close(loader);
}

/*
 * Once the lines have put the chip in its ROM, a failure resets it out again before closing -
 * RTS alone for the hold, then neither, as a finished write does - so the radio boots whatever
 * its flash holds rather than sitting in download mode until someone presses its button. A
 * failure in the reset itself, or one on a port whose lines were never moved, just closes.
 */
static void loader_fail(struct mesh_esp_loader *loader, enum mesh_esp_loader_error error,
                        uint64_t now_ms) {
    if (loader->resetting_out) {
        loader_failed(loader, now_ms);
        return;
    }
    const enum mesh_esp_loader_state was = loader->state;
    loader->error = error;
    inkwell_log_error("esp_loader", "Failed %s: %s%s%s, at %zu of %zu bytes",
                      mesh_esp_loader_state_name(was), mesh_esp_loader_error_name(error),
                      loader->reason[0] != '\0' ? ": " : "", loader->reason, loader->bytes_written,
                      loader->bytes_total);
    if (loader->port_open && loader->attempts > 0U &&
        inkwell_serial_set_lines(loader->fd, false, true) == 0) {
        loader->state = MESH_ESP_LOADER_RESTARTING;
        loader->resetting_out = true;
        loader->awaiting = 0U;
        loader->step_at_ms = now_ms + LOADER_RESET_HOLD_MS;
        return;
    }
    loader_failed(loader, now_ms);
}

static bool loader_lines(struct mesh_esp_loader *loader, bool dtr, bool rts, uint64_t now_ms) {
    const int result = inkwell_serial_set_lines(loader->fd, dtr, rts);
    if (result < 0) {
        snprintf(loader->reason, sizeof loader->reason, "control lines: %s", strerror(-result));
        loader_fail(loader, MESH_ESP_LOADER_ERROR_PORT, now_ms);
        return false;
    }
    return true;
}

/* ---- what we say ---------------------------------------------------------------------- */

/* Writes what is left of the request out. False once the conversation has failed. */
static bool loader_flush(struct mesh_esp_loader *loader, uint64_t now_ms) {
    while (loader->tx_sent < loader->tx_len) {
        const int wrote = inkwell_fd_write(loader->fd, loader->tx + loader->tx_sent,
                                           loader->tx_len - loader->tx_sent);
        if (wrote == -EAGAIN || wrote == -EWOULDBLOCK || wrote == -EINTR) {
            if (loader->watched) {
                (void)inkwell_loop_update_fd(loader->loop, loader->fd,
                                             INKWELL_LOOP_IN | INKWELL_LOOP_OUT);
            }
            return true;
        }
        if (wrote <= 0) {
            snprintf(loader->reason, sizeof loader->reason, "write: %s",
                     wrote < 0 ? strerror(-wrote) : "nothing written");
            loader_fail(loader, MESH_ESP_LOADER_ERROR_IO, now_ms);
            return false;
        }
        loader->tx_sent += (size_t)wrote;
    }
    if (loader->watched) {
        (void)inkwell_loop_update_fd(loader->loop, loader->fd, INKWELL_LOOP_IN);
    }
    return true;
}

/* Sends a request `build` wrote into tx, and waits `timeout_ms` for `op`'s answer. */
static void loader_send(struct mesh_esp_loader *loader, int built, uint8_t op, uint64_t timeout_ms,
                        uint64_t now_ms) {
    if (built < 0) {
        snprintf(loader->reason, sizeof loader->reason, "request 0x%02x: %s", op, strerror(-built));
        loader_fail(loader, MESH_ESP_LOADER_ERROR_IO, now_ms);
        return;
    }
    loader->tx_len = (size_t)built;
    loader->tx_sent = 0U;
    loader->awaiting = op;
    loader->deadline_ms = now_ms + timeout_ms;
    (void)loader_flush(loader, now_ms);
}

static void loader_sync(struct mesh_esp_loader *loader, uint64_t now_ms) {
    loader->step_at_ms = now_ms + LOADER_SYNC_INTERVAL_MS;
    loader_send(loader, inkwell_esp_rom_sync(loader->tx, sizeof loader->tx), INKWELL_ESP_ROM_SYNC,
                LOADER_SYNC_INTERVAL_MS, now_ms);
}

static void loader_reset_into_rom(struct mesh_esp_loader *loader, uint64_t now_ms) {
    loader->state = MESH_ESP_LOADER_RESETTING;
    loader->step = 0U;
    loader->attempts += 1U;
    loader->awaiting = 0U;
    /* The rate the ROM wakes at, whatever the last attempt left this side at. */
    if (loader->baud != INKWELL_ESP_ROM_BAUD) {
        (void)inkwell_serial_set_baud(loader->fd, INKWELL_ESP_ROM_BAUD);
        loader->baud = INKWELL_ESP_ROM_BAUD;
    }
    /* RTS alone: EN low, the chip held in reset. */
    if (loader_lines(loader, false, true, now_ms)) {
        loader->step_at_ms = now_ms + LOADER_RESET_HOLD_MS;
    }
}

static void loader_begin_region(struct mesh_esp_loader *loader, uint64_t now_ms) {
    const struct mesh_esp_loader_region *region = &loader->regions[loader->region];
    loader->state = MESH_ESP_LOADER_ERASING;
    loader->block = 0U;
    loader->erase_sent = true;
    inkwell_log_info("esp_loader", "Erasing %zu bytes at 0x%06x", region->len,
                     (unsigned)region->offset);
    loader_send(loader,
                inkwell_esp_rom_flash_begin(loader->chip, (uint32_t)region->len, region->offset,
                                            loader->tx, sizeof loader->tx),
                INKWELL_ESP_ROM_FLASH_BEGIN, per_mb(region->len, LOADER_ERASE_MS_PER_MB), now_ms);
}

static void loader_send_block(struct mesh_esp_loader *loader, uint64_t now_ms) {
    const struct mesh_esp_loader_region *region = &loader->regions[loader->region];
    const size_t at = (size_t)loader->block * INKWELL_ESP_ROM_BLOCK;
    const size_t left = region->len - at;
    const size_t len = left < INKWELL_ESP_ROM_BLOCK ? left : INKWELL_ESP_ROM_BLOCK;
    loader_send(loader,
                inkwell_esp_rom_flash_data(loader->block, region->data + at, len, loader->tx,
                                           sizeof loader->tx),
                INKWELL_ESP_ROM_FLASH_DATA, LOADER_COMMAND_TIMEOUT_MS, now_ms);
}

static void loader_restart(struct mesh_esp_loader *loader, uint64_t now_ms) {
    loader->state = MESH_ESP_LOADER_RESTARTING;
    loader->awaiting = 0U;
    /* RTS alone and then neither: a reset with the strap high, which boots the flash. */
    if (loader_lines(loader, false, true, now_ms)) {
        loader->step_at_ms = now_ms + LOADER_RESET_HOLD_MS;
    }
}

/* ---- what the ROM says ---------------------------------------------------------------- */

static void loader_answered(struct mesh_esp_loader *loader,
                            const struct inkwell_esp_rom_response *response, uint64_t now_ms) {
    if (loader->awaiting == 0U || response->op != loader->awaiting) {
        /* SYNC is answered eight times over, and a retry's answer can land after the one
           that was taken - neither is a question still open. */
        return;
    }
    loader->awaiting = 0U;
    if (response->status != 0U) {
        loader->rom_error = response->error;
        snprintf(loader->reason, sizeof loader->reason, "op 0x%02x: %s", response->op,
                 inkwell_esp_rom_error_name(response->error));
        loader_fail(loader, MESH_ESP_LOADER_ERROR_REFUSED, now_ms);
        return;
    }

    switch (loader->state) {
    case MESH_ESP_LOADER_SYNCING:
        loader->state = MESH_ESP_LOADER_IDENTIFYING;
        loader_send(
            loader,
            inkwell_esp_rom_read_reg(INKWELL_ESP_ROM_CHIP_MAGIC_REG, loader->tx, sizeof loader->tx),
            INKWELL_ESP_ROM_READ_REG, LOADER_COMMAND_TIMEOUT_MS, now_ms);
        return;
    case MESH_ESP_LOADER_IDENTIFYING: {
        uint16_t chip = 0U;
        if (!inkwell_esp_rom_chip_for_magic(response->value, &chip)) {
            snprintf(loader->reason, sizeof loader->reason, "unknown chip magic 0x%08x",
                     (unsigned)response->value);
            loader_fail(loader, MESH_ESP_LOADER_ERROR_WRONG_CHIP, now_ms);
            return;
        }
        loader->chip = chip;
        if (loader->expect_chip != MESH_ESP_LOADER_ANY_CHIP && chip != loader->expect_chip) {
            snprintf(loader->reason, sizeof loader->reason, "chip 0x%04x, images for 0x%04x",
                     (unsigned)chip, (unsigned)loader->expect_chip);
            loader_fail(loader, MESH_ESP_LOADER_ERROR_WRONG_CHIP, now_ms);
            return;
        }
        inkwell_log_info("esp_loader", "ROM answered: chip 0x%04x", (unsigned)chip);
        loader->state = MESH_ESP_LOADER_BAUD;
        loader_send(
            loader,
            inkwell_esp_rom_change_baud(MESH_ESP_LOADER_WRITE_BAUD, loader->tx, sizeof loader->tx),
            INKWELL_ESP_ROM_CHANGE_BAUDRATE, LOADER_COMMAND_TIMEOUT_MS, now_ms);
        return;
    }
    case MESH_ESP_LOADER_BAUD: {
        /* The answer came at the old rate; everything after it is at the new one. */
        const int result = inkwell_serial_set_baud(loader->fd, MESH_ESP_LOADER_WRITE_BAUD);
        if (result < 0) {
            snprintf(loader->reason, sizeof loader->reason, "baud %u: %s",
                     MESH_ESP_LOADER_WRITE_BAUD, strerror(-result));
            loader_fail(loader, MESH_ESP_LOADER_ERROR_PORT, now_ms);
            return;
        }
        loader->baud = MESH_ESP_LOADER_WRITE_BAUD;
        inkwell_esp_rom_reader_reset(&loader->reader);
        loader->state = MESH_ESP_LOADER_ATTACHING;
        loader->step = 0U;
        loader->step_at_ms = now_ms + LOADER_BAUD_SETTLE_MS;
        return;
    }
    case MESH_ESP_LOADER_ATTACHING:
        if (response->op == INKWELL_ESP_ROM_SPI_ATTACH) {
            loader_send(
                loader,
                inkwell_esp_rom_spi_set_params(loader->flash_size, loader->tx, sizeof loader->tx),
                INKWELL_ESP_ROM_SPI_SET_PARAMS, LOADER_COMMAND_TIMEOUT_MS, now_ms);
            return;
        }
        loader->region = 0U;
        loader_begin_region(loader, now_ms);
        return;
    case MESH_ESP_LOADER_ERASING: {
        const struct mesh_esp_loader_region *region = &loader->regions[loader->region];
        if (region->data != NULL) {
            loader->state = MESH_ESP_LOADER_WRITING;
            loader_send_block(loader, now_ms);
            return;
        }
        /* Erased and nothing to write: the hash is of a blank region. */
        loader->state = MESH_ESP_LOADER_VERIFYING;
        loader_send(loader,
                    inkwell_esp_rom_flash_md5(region->offset, (uint32_t)region->len, loader->tx,
                                              sizeof loader->tx),
                    INKWELL_ESP_ROM_SPI_FLASH_MD5, per_mb(region->len, LOADER_MD5_MS_PER_MB),
                    now_ms);
        return;
    }
    case MESH_ESP_LOADER_WRITING: {
        const struct mesh_esp_loader_region *region = &loader->regions[loader->region];
        const size_t at = (size_t)loader->block * INKWELL_ESP_ROM_BLOCK;
        const size_t left = region->len - at;
        loader->bytes_written += left < INKWELL_ESP_ROM_BLOCK ? left : INKWELL_ESP_ROM_BLOCK;
        loader->block += 1U;
        if (loader->block < inkwell_esp_rom_blocks((uint32_t)region->len)) {
            loader_send_block(loader, now_ms);
            return;
        }
        loader->state = MESH_ESP_LOADER_VERIFYING;
        loader_send(loader,
                    inkwell_esp_rom_flash_md5(region->offset, (uint32_t)region->len, loader->tx,
                                              sizeof loader->tx),
                    INKWELL_ESP_ROM_SPI_FLASH_MD5, per_mb(region->len, LOADER_MD5_MS_PER_MB),
                    now_ms);
        return;
    }
    case MESH_ESP_LOADER_VERIFYING:
        if (!inkwell_esp_rom_md5_matches(response, loader->digests[loader->region])) {
            snprintf(loader->reason, sizeof loader->reason, "region at 0x%06x",
                     (unsigned)loader->regions[loader->region].offset);
            loader_fail(loader, MESH_ESP_LOADER_ERROR_VERIFY, now_ms);
            return;
        }
        inkwell_log_info("esp_loader", "Verified %zu bytes at 0x%06x",
                         loader->regions[loader->region].len,
                         (unsigned)loader->regions[loader->region].offset);
        if (loader->regions[loader->region].data == NULL) {
            loader->bytes_written +=
                loader->regions[loader->region].len / LOADER_ERASE_PROGRESS_SHARE;
        }
        loader->region += 1U;
        if (loader->region < loader->region_count) {
            loader_begin_region(loader, now_ms);
            return;
        }
        loader_restart(loader, now_ms);
        return;
    default:
        return;
    }
}

void mesh_esp_loader_pump(struct mesh_esp_loader *loader, uint64_t now_ms) {
    if (!mesh_esp_loader_busy(loader) || !loader->port_open) {
        return;
    }
    if (!loader_flush(loader, now_ms)) {
        return;
    }
    uint8_t buffer[512];
    for (;;) {
        const int got = inkwell_fd_read(loader->fd, buffer, sizeof buffer);
        if (got == -EAGAIN || got == -EWOULDBLOCK || got == -EINTR) {
            return;
        }
        if (got <= 0) {
            snprintf(loader->reason, sizeof loader->reason, "read: %s",
                     got < 0 ? strerror(-got) : "the port went away");
            loader_fail(loader, MESH_ESP_LOADER_ERROR_IO, now_ms);
            return;
        }
        size_t at = 0U;
        while (at < (size_t)got && mesh_esp_loader_busy(loader)) {
            struct inkwell_esp_rom_response response;
            bool ready = false;
            at += inkwell_esp_rom_reader_feed(&loader->reader, buffer + at, (size_t)got - at,
                                              &response, &ready);
            if (ready && loader->state != MESH_ESP_LOADER_RESETTING &&
                loader->state != MESH_ESP_LOADER_RESTARTING) {
                loader_answered(loader, &response, now_ms);
            }
        }
        if (!mesh_esp_loader_busy(loader)) {
            return;
        }
    }
}

static int loader_ready(int fd, uint32_t events, void *userdata) {
    (void)fd;
    (void)events;
    mesh_esp_loader_pump((struct mesh_esp_loader *)userdata, inkwell_time_monotonic_ms());
    return 0;
}

void mesh_esp_loader_tick(struct mesh_esp_loader *loader, uint64_t now_ms) {
    if (!mesh_esp_loader_busy(loader)) {
        return;
    }
    mesh_esp_loader_pump(loader, now_ms);
    if (!mesh_esp_loader_busy(loader)) {
        return;
    }

    switch (loader->state) {
    case MESH_ESP_LOADER_RESETTING:
        if (now_ms < loader->step_at_ms) {
            return;
        }
        if (loader->step == 0U) {
            /* DTR alone: EN released while the strap is held low - download mode. */
            if (loader_lines(loader, true, false, now_ms)) {
                loader->step = 1U;
                loader->step_at_ms = now_ms + LOADER_STRAP_HOLD_MS;
            }
            return;
        }
        if (!loader_lines(loader, false, false, now_ms)) {
            return;
        }
        inkwell_esp_rom_reader_reset(&loader->reader);
        loader->state = MESH_ESP_LOADER_SYNCING;
        loader->step = 0U;
        loader_sync(loader, now_ms);
        return;
    case MESH_ESP_LOADER_SYNCING:
        if (now_ms < loader->step_at_ms) {
            return;
        }
        loader->step += 1U;
        if (loader->step < LOADER_SYNC_TRIES) {
            loader_sync(loader, now_ms);
            return;
        }
        if (loader->attempts < LOADER_RESET_TRIES) {
            inkwell_log_info("esp_loader", "No answer to SYNC; resetting again");
            loader_reset_into_rom(loader, now_ms);
            return;
        }
        inkwell_str_copy(loader->reason, sizeof loader->reason, "no answer to SYNC");
        loader_fail(loader, MESH_ESP_LOADER_ERROR_SILENT, now_ms);
        return;
    case MESH_ESP_LOADER_ATTACHING:
        if (loader->step == 0U && loader->awaiting == 0U) {
            if (now_ms < loader->step_at_ms) {
                return;
            }
            loader->step = 1U;
            loader_send(loader, inkwell_esp_rom_spi_attach(loader->tx, sizeof loader->tx),
                        INKWELL_ESP_ROM_SPI_ATTACH, LOADER_COMMAND_TIMEOUT_MS, now_ms);
            return;
        }
        break;
    case MESH_ESP_LOADER_RESTARTING:
        if (now_ms < loader->step_at_ms) {
            return;
        }
        if (!loader_lines(loader, false, false, now_ms)) {
            return;
        }
        if (loader->resetting_out) {
            loader_failed(loader, now_ms);
            return;
        }
        loader->state = MESH_ESP_LOADER_DONE;
        loader->finished_ms = now_ms;
        inkwell_log_info("esp_loader", "Wrote and verified %zu region(s) in %llu ms; restarted",
                         loader->region_count, (unsigned long long)(now_ms - loader->started_ms));
        loader_close(loader);
        return;
    default:
        break;
    }

    if (loader->awaiting != 0U && now_ms >= loader->deadline_ms) {
        snprintf(loader->reason, sizeof loader->reason, "no answer to op 0x%02x", loader->awaiting);
        loader_fail(loader, MESH_ESP_LOADER_ERROR_TIMEOUT, now_ms);
    }
}

static uint32_t flash_size_for(const struct mesh_esp_loader_region *regions, size_t count) {
    uint64_t end = 0U;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t region_end = (uint64_t)regions[i].offset + regions[i].len;
        end = region_end > end ? region_end : end;
    }
    uint64_t size = LOADER_MIN_FLASH;
    while (size < end && size < (1ULL << 31)) {
        size <<= 1;
    }
    return (uint32_t)size;
}

int mesh_esp_loader_start(struct mesh_esp_loader *loader, struct inkwell_loop *loop,
                          const char *path, uint16_t expect_chip,
                          const struct mesh_esp_loader_region *regions, size_t region_count,
                          uint32_t flash_size, uint64_t now_ms) {
    if (loader == NULL || path == NULL || regions == NULL || region_count == 0U ||
        region_count > MESH_ESP_LOADER_REGIONS) {
        return -EINVAL;
    }
    for (size_t i = 0; i < region_count; ++i) {
        if (regions[i].len == 0U || regions[i].len > UINT32_MAX) {
            return -EINVAL;
        }
    }
    memset(loader, 0, sizeof *loader);
    loader->fd = -1;
    loader->loop = loop;
    loader->expect_chip = expect_chip;
    loader->region_count = region_count;
    loader->started_ms = now_ms;
    loader->flash_size = flash_size != 0U ? flash_size : flash_size_for(regions, region_count);
    for (size_t i = 0; i < region_count; ++i) {
        loader->regions[i] = regions[i];
        struct inkwell_md5 md5;
        inkwell_md5_init(&md5);
        if (regions[i].data != NULL) {
            loader->bytes_total += regions[i].len;
        } else {
            loader->bytes_total += regions[i].len / LOADER_ERASE_PROGRESS_SHARE;
        }
        if (regions[i].data != NULL) {
            inkwell_md5_update(&md5, regions[i].data, regions[i].len);
        } else {
            /* What an erased region reads back as. */
            uint8_t blank[256];
            memset(blank, 0xFF, sizeof blank);
            for (size_t done = 0U; done < regions[i].len; done += sizeof blank) {
                const size_t left = regions[i].len - done;
                inkwell_md5_update(&md5, blank, left < sizeof blank ? left : sizeof blank);
            }
        }
        inkwell_md5_final(&md5, loader->digests[i]);
    }

    const int fd = inkwell_serial_open(path, INKWELL_ESP_ROM_BAUD);
    if (fd < 0) {
        loader->state = MESH_ESP_LOADER_FAILED;
        loader->error = MESH_ESP_LOADER_ERROR_PORT;
        snprintf(loader->reason, sizeof loader->reason, "open %s: %s", path, strerror(-fd));
        inkwell_log_error("esp_loader", "%s", loader->reason);
        return fd;
    }
    loader->fd = fd;
    loader->port_open = true;
    loader->baud = INKWELL_ESP_ROM_BAUD;
    if (loop != NULL && inkwell_loop_add_fd(loop, fd, INKWELL_LOOP_IN, loader_ready, loader) == 0) {
        loader->watched = true;
    }
    inkwell_log_info("esp_loader", "Writing %zu bytes in %zu region(s) through %s",
                     loader->bytes_total, region_count, path);
    loader_reset_into_rom(loader, now_ms);
    return loader->state == MESH_ESP_LOADER_FAILED ? -EIO : 0;
}

void mesh_esp_loader_cancel(struct mesh_esp_loader *loader) {
    if (loader == NULL) {
        return;
    }
    /* A reset out on the way: RTS alone and then neither, with the strap high. Cancel cannot
       wait out the hold a failure does, and needs none - EN is low for as long as the second
       control transfer takes to follow the first, a millisecond or so, where the chip asks for
       50 us. Without it a chip stopped in download mode stays there, off the mesh. */
    if (loader->port_open && loader->attempts > 0U) {
        (void)inkwell_serial_set_lines(loader->fd, false, true);
        (void)inkwell_serial_set_lines(loader->fd, false, false);
    }
    if (loader->port_open || loader->watched) {
        loader_close(loader);
    }
    loader->resetting_out = false;
    if (mesh_esp_loader_busy(loader)) {
        loader->state = MESH_ESP_LOADER_IDLE;
    }
}
