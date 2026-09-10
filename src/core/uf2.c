#include "mesh/core/uf2.h"

#include <string.h>

/* Little-endian on the wire, read byte by byte: a UF2 comes off a network and out of a zip,
   so nothing here may depend on the host's alignment or byte order. */
static uint32_t uf2_u32(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
           ((uint32_t)at[3] << 24);
}

/*
 * The architectures that come up as a UF2 bootloader, and the family each one's images carry.
 *
 * A table rather than a prefix test, for the same reason firmware_catalog.c keeps one: the
 * prefixes lie. "rp2040" and "rp2350" share five characters and are different families, and
 * "esp32-s3" and "esp32-c3" share six and take different paths - one of them none.
 */
static const struct {
    const char *architecture;
    uint32_t family;
} k_families[] = {
    {"nrf52840", MESH_UF2_FAMILY_NRF52840},
    {"rp2040", MESH_UF2_FAMILY_RP2040},
    {"rp2350", MESH_UF2_FAMILY_RP2350},
};

uint32_t mesh_uf2_family_for_architecture(const char *architecture) {
    if (architecture == NULL) {
        return 0U;
    }
    for (size_t i = 0; i < sizeof k_families / sizeof k_families[0]; ++i) {
        if (strcmp(k_families[i].architecture, architecture) == 0) {
            return k_families[i].family;
        }
    }
    return 0U;
}

bool mesh_uf2_block_parse(const uint8_t *bytes, size_t len, struct mesh_uf2_block *out) {
    if (bytes == NULL || out == NULL || len < MESH_UF2_BLOCK_SIZE) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (uf2_u32(bytes) != MESH_UF2_MAGIC_START0 || uf2_u32(bytes + 4U) != MESH_UF2_MAGIC_START1 ||
        uf2_u32(bytes + MESH_UF2_BLOCK_SIZE - 4U) != MESH_UF2_MAGIC_END) {
        return false;
    }
    out->flags = uf2_u32(bytes + 8U);
    out->target_address = uf2_u32(bytes + 12U);
    out->payload_size = uf2_u32(bytes + 16U);
    out->block_no = uf2_u32(bytes + 20U);
    out->num_blocks = uf2_u32(bytes + 24U);
    /*
     * Word 7 is three different things depending on the flags - a file size, a family id, or
     * nothing - so it is only read as a family when the flag says it is one. Reading it
     * unconditionally is how a file-container UF2's *length* comes to be compared against a
     * chip's family id, and the numbers involved are large enough that the mismatch looks
     * deliberate.
     */
    out->has_family = (out->flags & MESH_UF2_FLAG_FAMILY_ID) != 0U;
    out->family_id = out->has_family ? uf2_u32(bytes + 28U) : 0U;
    out->skip = (out->flags & MESH_UF2_FLAG_NOT_MAIN_FLASH) != 0U;
    return true;
}

enum mesh_uf2_verdict mesh_uf2_validate(const uint8_t *image, size_t len, uint32_t expect_family,
                                        struct mesh_uf2_info *out) {
    struct mesh_uf2_info info;
    memset(&info, 0, sizeof info);
    info.contiguous = true;

    enum mesh_uf2_verdict verdict = MESH_UF2_OK;
    if (image == NULL || len < MESH_UF2_BLOCK_SIZE || (len % MESH_UF2_BLOCK_SIZE) != 0U) {
        verdict = MESH_UF2_NOT_UF2;
    } else {
        const size_t count = len / MESH_UF2_BLOCK_SIZE;
        bool addressed = false;
        uint32_t next_address = 0U;
        for (size_t i = 0; i < count; ++i) {
            struct mesh_uf2_block block;
            if (!mesh_uf2_block_parse(image + i * MESH_UF2_BLOCK_SIZE, MESH_UF2_BLOCK_SIZE,
                                      &block)) {
                /* The first block failing means this is not a UF2; a later one failing means
                   it is one and something is wrong with it. Two different rows. */
                verdict = i == 0U ? MESH_UF2_NOT_UF2 : MESH_UF2_MALFORMED;
                break;
            }
            if (block.payload_size > MESH_UF2_PAYLOAD_MAX) {
                verdict = MESH_UF2_MALFORMED;
                break;
            }
            /*
             * A block that runs off the end of the address space is not an image, and it is
             * the one arithmetic here that could wrap: both fields are 32-bit and both come
             * out of the file. Left in, the span a caller computes comes back negative and
             * enormous, which is a progress bar that never moves and a size check that passes.
             */
            if (block.target_address > UINT32_MAX - block.payload_size) {
                verdict = MESH_UF2_MALFORMED;
                break;
            }
            if (i == 0U) {
                info.has_family = block.has_family;
                info.family_id = block.family_id;
                info.num_blocks = block.num_blocks;
                /*
                 * The family is checked once, against the first block, and then every block is
                 * checked against the first - so a file that changes family halfway through is
                 * refused whichever end the caller's expectation matched.
                 */
                if (expect_family != 0U &&
                    (!block.has_family || block.family_id != expect_family)) {
                    verdict = MESH_UF2_WRONG_FAMILY;
                    break;
                }
            } else if (block.has_family != info.has_family || block.family_id != info.family_id) {
                verdict = MESH_UF2_WRONG_FAMILY;
                break;
            } else if (block.num_blocks != info.num_blocks) {
                verdict = MESH_UF2_MALFORMED;
                break;
            }
            if (block.block_no != (uint32_t)i) {
                verdict = MESH_UF2_OUT_OF_ORDER;
                break;
            }
            info.blocks++;
            if (block.skip) {
                continue;
            }
            if (!addressed) {
                info.first_address = block.target_address;
                addressed = true;
            } else if (block.target_address != next_address) {
                info.contiguous = false;
            }
            next_address = block.target_address + block.payload_size;
            info.last_address = next_address;
            info.payload_bytes += block.payload_size;
        }
        if (verdict == MESH_UF2_OK && info.blocks != info.num_blocks) {
            /*
             * A file with more blocks than it declares is as broken as one with fewer, and the
             * two are told apart nowhere else - but INCOMPLETE is the one a user can act on
             * ("it did not all arrive"), and a longer file cannot happen without the count
             * having changed, which was refused above. So the only way here is short.
             */
            verdict = MESH_UF2_INCOMPLETE;
        }
    }

    if (out != NULL) {
        *out = info;
    }
    return verdict;
}
