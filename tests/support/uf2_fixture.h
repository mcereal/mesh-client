#ifndef MESH_TEST_UF2_FIXTURE_H
#define MESH_TEST_UF2_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

/*
 * A whole UF2, derived from the committed one.
 *
 * `tests/data/t114_2.7.26.uf2` is a splice - blocks 0, 1, 2864 and 2865 of the real image - so
 * it deliberately does not validate: its third block is out of order and its first two are a
 * truncated download. Anything that needs a *complete* file takes the first few blocks and
 * rewrites the one field that says how long the file is, which leaves every other byte the one
 * the release published.
 */

/* Writes `value` where a UF2 block keeps `numBlocks` (offset 24, little endian). */
void mesh_test_uf2_set_num_blocks(uint8_t *block, uint32_t value);

/*
 * The first `blocks` blocks of the fixture as a complete image the validator accepts. Returns a
 * buffer the caller frees and sets `*out_len`, or NULL when the fixture is unreadable or holds
 * fewer blocks than were asked for.
 */
uint8_t *mesh_test_uf2_whole(size_t blocks, size_t *out_len);

#endif /* MESH_TEST_UF2_FIXTURE_H */
