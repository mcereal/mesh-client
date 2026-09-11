/*
 * The PNG decoder: what comes out of a map tile, and what a broken one gets instead.
 *
 * The decoder itself is Wuffs and is not this suite's subject - upstream tests it against a
 * corpus far larger than anything here, and the reason it was chosen is that it is memory-safe
 * by construction. What *is* ours is the configuration: which pixel format we ask for, how big
 * the buffers are, which sizes are refused, and which failures are told apart. Every case below
 * is about one of those.
 *
 * The colour case is the one worth reading twice. It checks the channel order against the
 * fixture's own palette rather than against values a working decoder produced, because a test
 * that asserts what the code already does cannot fail when the code changes - and a swizzle
 * handed the wrong format still decodes every tile successfully, in the wrong colours, on a
 * screen nobody in CI is looking at.
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
#define TILE_TRUECOLOUR_PNG MESH_TEST_DATA_DIR "/tile_truecolour.png"

/* Bigger than either fixture and smaller than the reader's own cap, so a test that grows a
   fixture finds out by failing to load it rather than by decoding half of one. */
#define TILE_FIXTURE_MAX (64U * 1024U)

static size_t tile_read_fixture(const char *path, uint8_t *out, size_t cap) {
    FILE *const file = fopen(path, "rb");
    if (file == NULL) {
        return 0U;
    }
    const size_t got = fread(out, 1U, cap, file);
    const bool whole = feof(file) != 0;
    fclose(file);
    return whole ? got : 0U;
}

/*
 * The fixture's own palette, read straight out of the file.
 *
 * Twenty lines of PNG chunk walking rather than a table of colours copied into this suite,
 * because the copied table is the thing being avoided: the claim under test is that the decoder
 * puts the file's colours in a stated order, and a claim checked against a constant somebody
 * typed is a claim about the typing.
 */
static size_t tile_read_palette(const uint8_t *png, size_t len, uint8_t *out, size_t cap) {
    size_t at = 8U; /* past the signature */
    while (at + 8U <= len) {
        const uint32_t chunk = ((uint32_t)png[at] << 24) | ((uint32_t)png[at + 1U] << 16) |
                               ((uint32_t)png[at + 2U] << 8) | (uint32_t)png[at + 3U];
        if (memcmp(png + at + 4U, "PLTE", 4U) == 0) {
            if (chunk > cap || at + 8U + chunk > len) {
                return 0U;
            }
            memcpy(out, png + at + 8U, chunk);
            return chunk;
        }
        at += 12U + (size_t)chunk;
    }
    return 0U;
}

/* A decoded tile, and the buffer it landed in. 256 KiB is too much for a case's stack. */
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
    tile->encoded_len = tile_read_fixture(path, tile->encoded, sizeof tile->encoded);
    if (tile->pixels == NULL || tile->encoded_len == 0U) {
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
 * A real tile decodes to a full, opaque picture.
 *
 * "Opaque" is a claim about the panel rather than about PNG: the Brick's display layer
 * composites with per-pixel alpha, so a tile that decoded with a zero alpha byte would be
 * perfectly correct pixels that draw as nothing at all. That is the failure worth a case,
 * because it looks identical to the map not being open.
 */
MESH_TEST_CASE(tile_image_decodes_a_real_tile, unit) {
    struct tile_decoded *const tile = tile_load(TILE_PALETTE_PNG);
    MESH_TEST_FAIL_IF(tile == NULL, "the palette fixture loads");

    const int decoded = mesh_map_tile_decode(tile->encoded, tile->encoded_len, tile->pixels,
                                             MESH_MAP_TILE_IMAGE_BYTES);
    MESH_TEST_FAIL_IF_CLEANUP(decoded != 0, tile_free(tile), "and decodes");

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
 * The channel order, checked against the file rather than against ourselves.
 *
 * Every pixel of an indexed PNG is by definition one of its palette entries, so every decoded
 * pixel's red, green and blue - read out of the byte positions mesh/map/tile_image.h promises
 * them in - has to appear in the PLTE chunk. What makes this a real check rather than a
 * tautology is a property of the fixture, recorded in tests/data/README.md: not one of its 31
 * entries has its red and blue also present swapped. So a decoder configured for RGBA where the
 * client promised BGRA fails this on every non-grey pixel in the tile, which is most of them -
 * where a test asserting "the middle pixel is #71787F" would simply be re-recorded by whoever
 * broke it.
 */
MESH_TEST_CASE(tile_image_colours_come_from_the_files_own_palette, unit) {
    struct tile_decoded *const tile = tile_load(TILE_PALETTE_PNG);
    MESH_TEST_FAIL_IF(tile == NULL, "the palette fixture loads");

    uint8_t palette[256U * 3U];
    const size_t palette_len =
        tile_read_palette(tile->encoded, tile->encoded_len, palette, sizeof palette);
    MESH_TEST_FAIL_IF_CLEANUP(palette_len == 0U || palette_len % 3U != 0U, tile_free(tile),
                              "the fixture carries a palette");

    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decode(tile->encoded, tile->encoded_len, tile->pixels,
                                                   MESH_MAP_TILE_IMAGE_BYTES) != 0,
                              tile_free(tile), "and decodes");

    /* The property the check rests on, asserted here rather than trusted from the README: a
       fixture regenerated one day into a palette that is symmetric under a red/blue swap would
       leave this case passing while testing nothing. */
    size_t symmetric = 0U;
    for (size_t i = 0U; i < palette_len; i += 3U) {
        if (palette[i] == palette[i + 2U]) {
            continue; /* a grey, which says nothing either way */
        }
        for (size_t j = 0U; j < palette_len; j += 3U) {
            if (palette[j] == palette[i + 2U] && palette[j + 1U] == palette[i + 1U] &&
                palette[j + 2U] == palette[i]) {
                ++symmetric;
                break;
            }
        }
    }
    MESH_TEST_FAIL_IF_CLEANUP(symmetric != 0U, tile_free(tile),
                              "and no entry of it is its own red/blue swap");

    for (size_t i = 0U; i < MESH_MAP_TILE_IMAGE_BYTES; i += MESH_MAP_TILE_PIXEL_BYTES) {
        const uint8_t blue = tile->pixels[i];
        const uint8_t green = tile->pixels[i + 1U];
        const uint8_t red = tile->pixels[i + 2U];
        bool found = false;
        for (size_t entry = 0U; entry < palette_len && !found; entry += 3U) {
            found = palette[entry] == red && palette[entry + 1U] == green &&
                    palette[entry + 2U] == blue;
        }
        MESH_TEST_FAIL_IF_CLEANUP(!found, tile_free(tile),
                                  "every decoded pixel is one of the file's own colours");
    }
    tile_free(tile);
    record_success(test_name);
}

/*
 * The other colour type a pack can hold.
 *
 * A palette PNG and a 24-bit one reach the swizzler by different paths inside the decoder - one
 * through a palette lookup, one straight - and a pack builder that stopped quantising would
 * switch the client onto the second without anything else changing. The roadmap keeps 24-bit as
 * the expensive bracket rather than ruling it out, so it has to work.
 */
MESH_TEST_CASE(tile_image_decodes_a_truecolour_tile, unit) {
    struct tile_decoded *const tile = tile_load(TILE_TRUECOLOUR_PNG);
    MESH_TEST_FAIL_IF(tile == NULL, "the truecolour fixture loads");

    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decode(tile->encoded, tile->encoded_len, tile->pixels,
                                                   MESH_MAP_TILE_IMAGE_BYTES) != 0,
                              tile_free(tile), "a 24-bit tile decodes too");
    for (size_t i = 0U; i < MESH_MAP_TILE_IMAGE_BYTES; i += MESH_MAP_TILE_PIXEL_BYTES) {
        MESH_TEST_FAIL_IF_CLEANUP(tile->pixels[i + 3U] != 0xFFU, tile_free(tile),
                                  "and is opaque throughout, having carried no alpha at all");
    }
    tile_free(tile);
    record_success(test_name);
}

/*
 * Everything that is not a tile, and the two different answers it gets.
 *
 * -EILSEQ and -ENOBUFS are kept apart on purpose: one is a file being broken and the other is
 * this client asking wrong. A pack lives on a card somebody pulled out of a laptop, so the first
 * is a Tuesday and the second is a bug, and a caller that could not tell them apart would either
 * log a bug report for a scratched card or swallow its own mistake as bad input.
 */
MESH_TEST_CASE(tile_image_refuses_what_is_not_a_tile, unit) {
    struct tile_decoded *const tile = tile_load(TILE_PALETTE_PNG);
    MESH_TEST_FAIL_IF(tile == NULL, "the palette fixture loads");

    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_map_tile_decode(NULL, 10U, tile->pixels, MESH_MAP_TILE_IMAGE_BYTES) != -EINVAL,
        tile_free(tile), "no bytes is -EINVAL");
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_map_tile_decode(tile->encoded, 0U, tile->pixels, MESH_MAP_TILE_IMAGE_BYTES) != -EINVAL,
        tile_free(tile), "and so is a length of nothing");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decode(tile->encoded, tile->encoded_len, NULL,
                                                   MESH_MAP_TILE_IMAGE_BYTES) != -EINVAL,
                              tile_free(tile), "and so is nowhere to put it");

    /* A buffer one byte short of a tile. Refused rather than filled as far as it goes, because
       a partly written tile is a picture with a torn edge and nothing downstream could tell. */
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decode(tile->encoded, tile->encoded_len, tile->pixels,
                                                   MESH_MAP_TILE_IMAGE_BYTES - 1U) != -ENOBUFS,
                              tile_free(tile), "a buffer short of a tile is -ENOBUFS");

    /* Not a PNG at all. */
    static const uint8_t k_not_png[] = "MCTPACK2 and then some bytes that are not an image";
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decode(k_not_png, sizeof k_not_png, tile->pixels,
                                                   MESH_MAP_TILE_IMAGE_BYTES) != -EILSEQ,
                              tile_free(tile), "bytes that are not a PNG are -EILSEQ");

    /*
     * A tile cut short - the shape a pack copied half way across produces, and the one a
     * streaming decoder gets wrong by waiting politely for the rest of a file that has no rest.
     */
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decode(tile->encoded, tile->encoded_len / 2U,
                                                   tile->pixels,
                                                   MESH_MAP_TILE_IMAGE_BYTES) != -EILSEQ,
                              tile_free(tile), "half a tile is -EILSEQ rather than a wait");

    /* And one with its image data corrupted but its length intact, which is what a bad sector
       under a file that was never re-read looks like. The header still parses; the zlib stream
       under it does not. */
    uint8_t *const damaged = malloc(tile->encoded_len);
    MESH_TEST_FAIL_IF_CLEANUP(damaged == NULL, tile_free(tile), "a copy to damage");
    memcpy(damaged, tile->encoded, tile->encoded_len);
    for (size_t i = tile->encoded_len / 2U; i < tile->encoded_len / 2U + 64U; ++i) {
        damaged[i] = (uint8_t)~damaged[i];
    }
    const int hurt =
        mesh_map_tile_decode(damaged, tile->encoded_len, tile->pixels, MESH_MAP_TILE_IMAGE_BYTES);
    free(damaged);
    MESH_TEST_FAIL_IF_CLEANUP(hurt != -EILSEQ, tile_free(tile),
                              "and damaged image data is -EILSEQ, not a picture of the damage");
    tile_free(tile);
    record_success(test_name);
}

/*
 * Decoding holds a constant amount of memory, and decoding more tiles does not hold more.
 *
 * The promise docs/maps-roadmap.md asked a decoder to prove, and the reason this one was picked
 * over an allocating library: a decode on an event loop cannot pause to find memory and cannot
 * fail for want of it. Ten decodes rather than one because the failure this guards against -
 * state that grows per call - is invisible in a single one.
 */
MESH_TEST_CASE(tile_image_holds_a_bounded_amount_of_memory, unit) {
    const size_t before = mesh_map_tile_decoder_bytes();
    /*
     * The two blocks named in src/map/tile_image.c and nothing else: 48 KiB of decoder and
     * 256 KiB of scratch, so a little over 304 KiB. The cap is a bound rather than the figure,
     * because the figure is upstream's to move a little - but a *large* move is the roadmap's
     * tile-cache budget being quietly rewritten by a decoder, which is worth failing over.
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

    /* A failed decode has to leave the next one working. The decoder is re-initialised per
       call for exactly this reason, and a state machine left mid-image by a broken file is the
       ordinary way that stops being true. */
    static const uint8_t k_rubbish[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    (void)mesh_map_tile_decode(k_rubbish, sizeof k_rubbish, tile->pixels,
                               MESH_MAP_TILE_IMAGE_BYTES);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_map_tile_decode(tile->encoded, tile->encoded_len, tile->pixels,
                                                   MESH_MAP_TILE_IMAGE_BYTES) != 0,
                              tile_free(tile), "and a broken tile does not poison the next one");
    tile_free(tile);
    record_success(test_name);
}
