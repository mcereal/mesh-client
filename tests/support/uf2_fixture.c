#include "support/uf2_fixture.h"

#include "support/data_fixture.h"

#include "mesh/core/uf2.h"

#include <stdlib.h>
#include <string.h>

void mesh_test_uf2_set_num_blocks(uint8_t *block, uint32_t value) {
    block[24] = (uint8_t)(value & 0xFFU);
    block[25] = (uint8_t)((value >> 8) & 0xFFU);
    block[26] = (uint8_t)((value >> 16) & 0xFFU);
    block[27] = (uint8_t)((value >> 24) & 0xFFU);
}

uint8_t *mesh_test_uf2_whole(size_t blocks, size_t *out_len) {
    if (blocks == 0U) {
        return NULL;
    }
    size_t len = 0U;
    char *const bytes = mesh_test_data_read("t114_2.7.26.uf2", &len);
    if (bytes == NULL) {
        return NULL;
    }
    const size_t wanted = blocks * MESH_UF2_BLOCK_SIZE;
    if (len < wanted) {
        free(bytes);
        return NULL;
    }
    uint8_t *const image = malloc(wanted);
    if (image == NULL) {
        free(bytes);
        return NULL;
    }
    memcpy(image, bytes, wanted);
    free(bytes);
    for (size_t i = 0; i < blocks; ++i) {
        mesh_test_uf2_set_num_blocks(image + (i * MESH_UF2_BLOCK_SIZE), (uint32_t)blocks);
    }
    if (out_len != NULL) {
        *out_len = wanted;
    }
    return image;
}
