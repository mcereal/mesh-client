/*
 * The tile-shaped seam over inkwell's PNG decoder.
 *
 * What a PNG decoder does with bytes is not this suite's subject any more - inkwell's codec_png
 * suite holds the channel order, the colour types, the refusals and the bounded memory, against
 * the same fixtures. What is left here is the part that is about a *tile*: that this client
 * asks for its own size, that a tile lands in exactly the block a cache budgets for one, and
 * that the two static buffers behind the seam are the footprint the tile cache was sized
 * against.
 *
 * Which is the whole point of the split. A change to the decoder fails in inkwell; a change to
 * what this client asks it for fails here.
 */

#include "framework/mesh_test.h"

#include "mesh/map/tile_image.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MESH_TEST_DATA_DIR
#define MESH_TEST_DATA_DIR "tests/data"
#endif

#define TILE_PALETTE_PNG MESH_TEST_DATA_DIR "/tile_palette.png"

/* Bigger than the fixture and smaller than the reader's own cap, so a test that grows a
   fixture finds out by failing to load it rather than by decoding half of one. */
#define TILE_FIXTURE_MAX (64U * 1024U)

struct tile_decoded {
    uint8_t *pixels;
    uint8_t encoded[TILE_FIXTURE_MAX];
    size_t encoded_len;
};

static struct tile_decoded *tile_load(const char *path) {
    struct tile_decoded *const tile = calloc(1U, sizeof *tile);
    if (tile == NULL) {
        return NULL;
    }
    tile->pixels = malloc(MESH_MAP_TILE_IMAGE_BYTES);
    FILE *const file = fopen(path, "rb");
    if (tile->pixels == NULL || file == NULL) {
        free(tile->pixels);
        free(tile);
        return NULL;
    }
    tile->encoded_len = fread(tile->encoded, 1U, sizeof tile->encoded, file);
    const bool whole = feof(file) != 0;
    (void)fclose(file);
    if (!whole || tile->encoded_len == 0U) {
        free(tile->pixels);
        free(tile);
        return NULL;
    }
    return tile;
}

static void tile_free(struct tile_decoded *tile) {
    if (tile != NULL) {
        free(tile->pixels);
        free(tile);
    }
}

/*
 * A real tile fills a real tile's worth of buffer, opaque throughout.
 *
 * "Opaque" is a claim about the panel rather than about PNG: the Brick's display layer
 * composites with per-pixel alpha, so a tile that decoded with a zero alpha byte would be
 * perfectly correct pixels that draw as nothing at all. That is the failure worth a case,
 * because it looks identical to the map not being open - and it is the pixel format this seam
 * asks for that decides it.
 */
MESH_TEST_CASE(tile_image_decodes_a_real_tile, unit) {
    struct tile_decoded *const tile = tile_load(TILE_PALETTE_PNG);
    MESH_TEST_FAIL_IF(tile == NULL, "the palette fixture loads");

    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decode(tile->encoded, tile->encoded_len, tile->pixels,
                                                   MESH_MAP_TILE_IMAGE_BYTES) != 0,
                              tile_free(tile), "a tile decodes");

    size_t opaque = 0U;
    uint32_t distinct = 0U;
    uint32_t first = 0U;
    for (size_t i = 0U; i < MESH_MAP_TILE_IMAGE_BYTES; i += MESH_MAP_TILE_PIXEL_BYTES) {
        if (tile->pixels[i + 3U] == 0xFFU) {
            ++opaque;
        }
        const uint32_t colour = (uint32_t)tile->pixels[i] | ((uint32_t)tile->pixels[i + 1U] << 8) |
                                ((uint32_t)tile->pixels[i + 2U] << 16);
        if (i == 0U) {
            first = colour;
        } else if (colour != first) {
            distinct = 1U;
        }
    }
    MESH_TEST_FAIL_IF_CLEANUP(opaque != (size_t)MESH_MAP_TILE_SIZE * MESH_MAP_TILE_SIZE,
                              tile_free(tile), "every pixel is opaque");
    /* And it is a picture rather than a fill - a decode that wrote one colour over the whole
       buffer would pass every other assertion here. */
    MESH_TEST_FAIL_IF_CLEANUP(distinct == 0U, tile_free(tile), "and is not one flat colour");
    tile_free(tile);
    record_success(test_name);
}

/*
 * The size this client asks for is the size it budgets for, in both directions.
 *
 * The seam passes MESH_MAP_TILE_SIZE down and the decoder refuses anything else, so a pack that
 * held 512-square tiles would be a stated -EINVAL rather than a buffer overrun - and a buffer
 * one byte short of a tile is refused rather than filled as far as it goes, because a partly
 * written tile is a picture with a torn edge and nothing downstream could tell.
 *
 * This is the case that fails if somebody changes MESH_MAP_TILE_SIZE without changing what a
 * pack builder emits, which is the one mistake the seam makes possible.
 */
MESH_TEST_CASE(tile_image_holds_this_clients_tile_size, unit) {
    struct tile_decoded *const tile = tile_load(TILE_PALETTE_PNG);
    MESH_TEST_FAIL_IF(tile == NULL, "the palette fixture loads");

    MESH_TEST_FAIL_IF_CLEANUP(MESH_MAP_TILE_IMAGE_BYTES != (size_t)MESH_MAP_TILE_SIZE *
                                                               MESH_MAP_TILE_SIZE *
                                                               MESH_MAP_TILE_PIXEL_BYTES,
                              tile_free(tile), "a tile's block is its size times its pixels");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decode(tile->encoded, tile->encoded_len, tile->pixels,
                                                   MESH_MAP_TILE_IMAGE_BYTES - 1U) != -ENOBUFS,
                              tile_free(tile), "a buffer short of a tile is -ENOBUFS");
    /* The fixture is this client's tile size, which is what makes the case above mean
       anything: a fixture of some other size would fail the decode for the wrong reason. */
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decode(tile->encoded, tile->encoded_len, tile->pixels,
                                                   MESH_MAP_TILE_IMAGE_BYTES) != 0,
                              tile_free(tile), "and the fixture really is a tile of that size");
    tile_free(tile);
    record_success(test_name);
}

/*
 * Decoding holds a constant amount of memory, and it is the amount the tile cache was budgeted
 * against.
 *
 * The buffers are this client's now - inkwell's decoder takes both from its caller - so this is
 * the one place the figure exists. Ten decodes rather than one because the failure this guards
 * against, state that grows per call, is invisible in a single one.
 */
MESH_TEST_CASE(tile_image_holds_a_bounded_amount_of_memory, unit) {
    const size_t before = mesh_map_tile_decoder_bytes();
    /*
     * 48 KiB of decoder state and 256 KiB of scratch, so a little over 304 KiB. The cap is a
     * bound rather than the figure, because the figure is upstream's to move a little - but a
     * *large* move is the roadmap's tile-cache budget being quietly rewritten by a decoder,
     * which is worth failing over.
     */
    MESH_TEST_FAIL_IF(before == 0U || before > 384U * 1024U,
                      "the decoder's footprint is stated and bounded");

    struct tile_decoded *const tile = tile_load(TILE_PALETTE_PNG);
    MESH_TEST_FAIL_IF(tile == NULL, "the palette fixture loads");
    for (unsigned round = 0U; round < 10U; ++round) {
        MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decode(tile->encoded, tile->encoded_len,
                                                       tile->pixels,
                                                       MESH_MAP_TILE_IMAGE_BYTES) != 0,
                                  tile_free(tile), "a tile decodes every time it is asked");
    }
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decoder_bytes() != before, tile_free(tile),
                              "and the tenth costs what the first did");
    tile_free(tile);
    record_success(test_name);
}

/* A pixel of a synthetic tile that says where it came from: its column in blue, its row in
   green, so an enlarged copy can be checked against the square it was cut from. */
static void tile_mark(uint8_t *pixels) {
    for (size_t row = 0; row < MESH_MAP_TILE_SIZE; ++row) {
        for (size_t column = 0; column < MESH_MAP_TILE_SIZE; ++column) {
            uint8_t *const at =
                pixels + (row * MESH_MAP_TILE_SIZE + column) * MESH_MAP_TILE_PIXEL_BYTES;
            at[0] = (uint8_t)column;
            at[1] = (uint8_t)row;
            at[2] = 0x5AU;
            at[3] = 0xFFU;
        }
    }
}

/*
 * A deeper tile is its own quarter (or sixteenth...) of the ancestor, each pixel repeated.
 *
 * Checked at every pixel against where it has to have come from, at one level and at the most
 * the client draws, and for a child in the ancestor's bottom-right corner so a swapped x and y
 * or a lost offset cannot pass by landing in the top-left.
 */
MESH_TEST_CASE(tile_image_overzoom_enlarges_the_childs_square, unit) {
    uint8_t *const ancestor = malloc(MESH_MAP_TILE_IMAGE_BYTES);
    uint8_t *const child = malloc(MESH_MAP_TILE_IMAGE_BYTES);
    MESH_TEST_FAIL_IF(ancestor == NULL || child == NULL, "two tiles' memory");
    tile_mark(ancestor);

    const struct mesh_map_tile_key above = {.zoom = 13U, .x = 2641U, .y = 3606U};
    for (unsigned levels = 1U; levels <= MESH_MAP_TILE_OVERZOOM_LEVELS; levels += 3U) {
        const size_t repeat = (size_t)1 << levels;
        const size_t part = MESH_MAP_TILE_SIZE / repeat;
        const struct mesh_map_tile_key key = {.zoom = (uint8_t)(above.zoom + levels),
                                              .x = (above.x << levels) + (uint32_t)repeat - 1U,
                                              .y = (above.y << levels) + (uint32_t)repeat - 2U};
        MESH_TEST_FAIL_IF(
            mesh_map_tile_overzoom(ancestor, above, key, child, MESH_MAP_TILE_IMAGE_BYTES) != 0,
            "a child within reach is drawn");
        for (size_t row = 0; row < MESH_MAP_TILE_SIZE; ++row) {
            for (size_t column = 0; column < MESH_MAP_TILE_SIZE; ++column) {
                const uint8_t *const at =
                    child + (row * MESH_MAP_TILE_SIZE + column) * MESH_MAP_TILE_PIXEL_BYTES;
                const size_t from_column = (repeat - 1U) * part + column / repeat;
                const size_t from_row = (repeat - 2U) * part + row / repeat;
                MESH_TEST_FAIL_IF(at[0] != (uint8_t)from_column || at[1] != (uint8_t)from_row ||
                                      at[2] != 0x5AU || at[3] != 0xFFU,
                                  "every pixel is the ancestor's pixel above it");
            }
        }
    }
    free(ancestor);
    free(child);
    record_success(test_name);
}

/* What is not an ancestor within reach is refused, and nothing is guessed from it. */
MESH_TEST_CASE(tile_image_overzoom_refuses_what_is_not_above_it, unit) {
    uint8_t *const ancestor = calloc(1U, MESH_MAP_TILE_IMAGE_BYTES);
    uint8_t *const child = calloc(1U, MESH_MAP_TILE_IMAGE_BYTES);
    MESH_TEST_FAIL_IF(ancestor == NULL || child == NULL, "two tiles' memory");

    const struct mesh_map_tile_key above = {.zoom = 13U, .x = 100U, .y = 200U};
    const struct mesh_map_tile_key inside = {.zoom = 14U, .x = 201U, .y = 401U};
    const struct mesh_map_tile_key beside = {.zoom = 14U, .x = 202U, .y = 401U};
    const struct mesh_map_tile_key too_deep = {
        .zoom = (uint8_t)(13U + MESH_MAP_TILE_OVERZOOM_LEVELS + 1U),
        .x = 100U << (MESH_MAP_TILE_OVERZOOM_LEVELS + 1U),
        .y = 200U << (MESH_MAP_TILE_OVERZOOM_LEVELS + 1U)};

    MESH_TEST_FAIL_IF(mesh_map_tile_overzoom(ancestor, above, beside, child,
                                             MESH_MAP_TILE_IMAGE_BYTES) != -EINVAL,
                      "a neighbour's child is not this tile's");
    MESH_TEST_FAIL_IF(
        mesh_map_tile_overzoom(ancestor, above, above, child, MESH_MAP_TILE_IMAGE_BYTES) != -EINVAL,
        "a tile is not drawn from itself");
    MESH_TEST_FAIL_IF(mesh_map_tile_overzoom(ancestor, inside, above, child,
                                             MESH_MAP_TILE_IMAGE_BYTES) != -EINVAL,
                      "nor from its own child");
    MESH_TEST_FAIL_IF(mesh_map_tile_overzoom(ancestor, above, too_deep, child,
                                             MESH_MAP_TILE_IMAGE_BYTES) != -EINVAL,
                      "past the reach the client draws, nothing is");
    MESH_TEST_FAIL_IF(mesh_map_tile_overzoom(ancestor, above, inside, child,
                                             MESH_MAP_TILE_IMAGE_BYTES - 1U) != -ENOBUFS,
                      "a buffer short of a tile is refused");
    MESH_TEST_FAIL_IF(
        mesh_map_tile_overzoom(ancestor, above, inside, child, MESH_MAP_TILE_IMAGE_BYTES) != 0,
        "and the child it does hold is drawn");
    free(ancestor);
    free(child);
    record_success(test_name);
}
