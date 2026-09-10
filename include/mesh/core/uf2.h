#pragma once

/*
 * Reading a UF2 file - the format the other half of this feature hands to a bootloader.
 *
 * A UF2 is 512-byte records and nothing else: no header, no index, no trailer. Each record
 * carries its own address, its own position in the file and the file's own length, which is
 * what makes the format writable to a device that has no idea what a filesystem is. The
 * Adafruit nRF52 bootloader really does ignore the FAT it presents, watch every 512-byte block
 * that goes past, and flash the ones carrying the magic - so a `.uf2` written to its mass
 * storage in any order, through any layer, arrives.
 *
 * That is exactly why this file exists. When the bytes are their own protocol there is no
 * envelope to get wrong and no handshake to fail: the only defence against writing a T-Beam
 * image onto a T114 is reading what the blocks say before writing them. The family id is that
 * check, it is in **every** block rather than in a header somebody could strip, and it is
 * hardware-enforced on the far side too - a bootloader refuses a family that is not its own.
 * Checking it here is what turns a refusal into a row rather than into a board that reboots
 * into nothing.
 *
 * Pure, like the catalog: bytes in, verdict out, no file handles and no device.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed, and it is the whole of the format's framing. */
#define MESH_UF2_BLOCK_SIZE 512U
/*
 * The most one block *may* carry. Every Meshtastic release image measured carries **256**, and
 * pinning that matters more than it looks: an arithmetic written against the maximum is wrong
 * by 46% and still produces a plausible-looking number - a progress bar that reaches 54% and
 * stops, or a size check that passes a file half the length it should be.
 */
#define MESH_UF2_PAYLOAD_MAX 476U

#define MESH_UF2_MAGIC_START0 0x0A324655U /* "UF2\n" */
#define MESH_UF2_MAGIC_START1 0x9E5D5157U
#define MESH_UF2_MAGIC_END 0x0AB16F30U

/* This block is not for the main flash - a bootloader skips it, and so do we. */
#define MESH_UF2_FLAG_NOT_MAIN_FLASH 0x00000001U
/* `file_size` is a family id rather than a length. Set on every release image measured. */
#define MESH_UF2_FLAG_FAMILY_ID 0x00002000U

/*
 * Family ids, **measured** off real 2.7.26 release images rather than read off a table: the
 * nrf52840 zip's T114 `.uf2`, the rp2040 zip's `rp2040-lora`, and the rp2350 zip's `pico2w`.
 * The RP2350 has more than one published family - secure, non-secure, RISC-V - and upstream
 * builds the ARM secure one, which is not a thing to guess about when the failure is a board
 * that has taken an image its bootloader will not start.
 */
#define MESH_UF2_FAMILY_NRF52840 0xADA52840U
#define MESH_UF2_FAMILY_RP2040 0xE48BFF56U
#define MESH_UF2_FAMILY_RP2350 0xE48BFF59U

/*
 * Why a file was refused. Each of these is a different sentence on a screen, which is why they
 * are told apart rather than collapsed into false.
 */
enum mesh_uf2_verdict {
    MESH_UF2_OK = 0,
    /* Not a length that could be 512-byte records, or the first record has no magic. This is
       "what came out of the zip is not a UF2 at all". */
    MESH_UF2_NOT_UF2,
    /* Records, but one of them is wrong: no magic mid-file, a payload longer than a block can
       hold, or a `numBlocks` that changed halfway through. */
    MESH_UF2_MALFORMED,
    /* A UF2 for a different chip. The one refusal that protects the board rather than the
       download. */
    MESH_UF2_WRONG_FAMILY,
    /* `blockNo` is not the sequence 0..n-1. Every generator writes them in order and the
       bootloader counts on it; a file that is not is one we cannot report progress against. */
    MESH_UF2_OUT_OF_ORDER,
    /* Fewer records than the records themselves say there should be - a truncated download. */
    MESH_UF2_INCOMPLETE,
    MESH_UF2_VERDICT_COUNT,
};

/* One 512-byte record, as it describes itself. */
struct mesh_uf2_block {
    uint32_t flags;
    uint32_t target_address;
    uint32_t payload_size;
    uint32_t block_no;
    uint32_t num_blocks;
    /* Only meaningful with `has_family`; 0 otherwise, which is also a legal family id, hence
       the flag rather than a sentinel. */
    uint32_t family_id;
    bool has_family;
    /* MESH_UF2_FLAG_NOT_MAIN_FLASH, pulled out because it is the one flag that changes what a
       caller does with the block. */
    bool skip;
};

/* What a whole file turned out to be. Filled in as far as the walk got, so it is still worth
   reading after a verdict that is not OK. */
struct mesh_uf2_info {
    uint32_t family_id;
    bool has_family;
    /* What the records claim, and how many were actually there. Equal on a whole file. */
    uint32_t num_blocks;
    uint32_t blocks;
    /* Payload bytes across every block that is not a skip: what will actually be flashed, and
       not the same number as the file's length. */
    uint64_t payload_bytes;
    uint32_t first_address;
    /* The address one past the last payload byte, so `last_address - first_address` is the
       span - which equals `payload_bytes` exactly when the file is contiguous. */
    uint32_t last_address;
    /* Every block's address followed the one before it with no gap. True for every release
       image measured; false is not a refusal, because a legal UF2 may skip regions. */
    bool contiguous;
};

/*
 * Reads one 512-byte record. False when it is short or carries no magic - which is not
 * necessarily an error: a block without the magic is discarded by the bootloader and reported
 * as written, which is the property that lets the write path be exercised at full size against
 * a real board without touching its flash.
 */
bool mesh_uf2_block_parse(const uint8_t *bytes, size_t len, struct mesh_uf2_block *out);

/*
 * Walks a whole `.uf2` and says whether it is one, and whether it is for `expect_family`.
 *
 * `expect_family` of 0 accepts whatever the file says and reports it, which is what a test or
 * an inspection wants; the install path passes the family for the board it is connected to,
 * and that is the guard the roadmap describes as "much harder to get past by accident" than
 * asking the user which variant they have.
 *
 * `out` may be NULL. The walk stops at the first block it refuses, so `out->blocks` says where.
 */
enum mesh_uf2_verdict mesh_uf2_validate(const uint8_t *image, size_t len, uint32_t expect_family,
                                        struct mesh_uf2_info *out);

/*
 * The UF2 family for an architecture as `deviceHardware` spells it, or 0.
 *
 * 0 means "no UF2 path", which is every ESP32 and portduino - not "any family will do". A
 * caller that passed 0 through to mesh_uf2_validate() as `expect_family` would be turning off
 * the one check that protects the board, so the install path tests this first.
 */
uint32_t mesh_uf2_family_for_architecture(const char *architecture);

#ifdef __cplusplus
}
#endif
