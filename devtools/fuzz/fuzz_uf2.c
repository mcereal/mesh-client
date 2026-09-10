/*
 * The file a bootloader is about to be handed, fed whatever arrives.
 *
 * A UF2 is fixed-size records with a self-declared payload length inside each one, which is the
 * shape that goes wrong: the record is 512 bytes and the field says up to 476, and every
 * arithmetic downstream - how much is being flashed, how far along a bar is, whether the file
 * is complete - is built on a number the file chose. It is also the last thing between a
 * download and a radio's flash, so a reader that accepted the wrong file here is not a crash,
 * it is a brick.
 *
 * Build with scripts/fuzz.sh; see docs/testing.md.
 */

#include "mesh/core/uf2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fuzz_broke(const char *what) {
    fprintf(stderr, "uf2 contract broken: %s\n", what);
    fflush(stderr);
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct mesh_uf2_block block;
    if (mesh_uf2_block_parse(data, size, &block)) {
        if (block.has_family != ((block.flags & MESH_UF2_FLAG_FAMILY_ID) != 0U)) {
            fuzz_broke("a family read from a block that did not declare one");
        }
        if (block.skip != ((block.flags & MESH_UF2_FLAG_NOT_MAIN_FLASH) != 0U)) {
            fuzz_broke("the skip flag should be the flag and nothing else");
        }
    }

    struct mesh_uf2_info info;
    /*
     * Both sides of the guard: 0 accepts whatever the file claims, and a real family is what
     * the install path passes. The second must never accept a file the first reported as
     * something else - that is the whole of the protection, stated as a contract.
     */
    const enum mesh_uf2_verdict open = mesh_uf2_validate(data, size, 0U, &info);
    if (open == MESH_UF2_OK) {
        if (info.blocks != info.num_blocks) {
            fuzz_broke("a valid file whose block count does not match its own declaration");
        }
        if (info.blocks != size / MESH_UF2_BLOCK_SIZE) {
            fuzz_broke("a valid file that did not account for every record in it");
        }
        if (info.payload_bytes > (uint64_t)info.blocks * MESH_UF2_PAYLOAD_MAX) {
            fuzz_broke("more payload than the records could carry");
        }
        if (info.contiguous && info.payload_bytes != 0U &&
            (uint64_t)info.last_address - (uint64_t)info.first_address != info.payload_bytes) {
            fuzz_broke("a contiguous file whose span is not its payload");
        }
        static const uint32_t k_families[] = {MESH_UF2_FAMILY_NRF52840, MESH_UF2_FAMILY_RP2040,
                                              MESH_UF2_FAMILY_RP2350};
        for (size_t i = 0; i < sizeof k_families / sizeof k_families[0]; ++i) {
            const enum mesh_uf2_verdict aimed = mesh_uf2_validate(data, size, k_families[i], NULL);
            const bool matches = info.has_family && info.family_id == k_families[i];
            if (matches && aimed != MESH_UF2_OK) {
                fuzz_broke("a file refused for the family it declares");
            }
            if (!matches && aimed != MESH_UF2_WRONG_FAMILY) {
                fuzz_broke("a file accepted for a family it does not declare");
            }
        }
    }

    /* The architecture table, over strings that came out of the input rather than ones we
       chose - a lookup with no length is the other way this goes wrong. */
    if (size > 1U) {
        char name[24];
        const size_t take = size - 1U < sizeof name - 1U ? size - 1U : sizeof name - 1U;
        memcpy(name, data + 1U, take);
        name[take] = '\0';
        (void)mesh_uf2_family_for_architecture(name);
    }
    return 0;
}
