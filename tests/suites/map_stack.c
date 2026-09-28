/*
 * Several packs drawn as one map: which pack answers for a tile, and which files a maps
 * directory is.
 *
 * The rule is arithmetic over indexes in RAM, so every case here is a handful of keys written as
 * real packs and a question about which one wins - no framebuffer, no decode. The picture that
 * rule produces is the capture suite's business; what can be wrong here is the *choice*, and a
 * wrong choice draws a perfectly good tile from the wrong file, which no screenshot says.
 */

#include "framework/mesh_test.h"

#include "support/map_fixture.h"

#include "mesh/map/stack.h"
#include "mesh/map/tile_image.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A world base to zoom 2: every tile there is at 0, 1 and 2. */
static size_t stack_world_keys(struct mesh_map_tile_key *out) {
    size_t count = 0U;
    for (uint8_t zoom = 0U; zoom <= 2U; ++zoom) {
        for (uint32_t x = 0U; x < (1U << zoom); ++x) {
            for (uint32_t y = 0U; y < (1U << zoom); ++y) {
                out[count++] = (struct mesh_map_tile_key){.zoom = zoom, .x = x, .y = y};
            }
        }
    }
    return count;
}

/* A region to zoom 4: one tile a level, the chain a pipeline writes above a small region. */
static const struct mesh_map_tile_key k_region[] = {
    {0, 0, 0}, {1, 0, 0}, {2, 0, 1}, {3, 1, 2}, {4, 2, 4},
};

struct stack_fixture {
    struct mesh_test_map_pack world;
    struct mesh_test_map_pack region;
    struct mesh_map_stack stack;
};

static bool stack_fixture_open(struct stack_fixture *fixture) {
    memset(fixture, 0, sizeof *fixture);
    struct mesh_map_tile_key world[21];
    const size_t world_count = stack_world_keys(world);
    if (mesh_test_map_pack_write(&fixture->world, world, world_count, 0U, "World",
                                 "(c) OpenStreetMap contributors") != 0 ||
        mesh_test_map_pack_write(&fixture->region, k_region, sizeof k_region / sizeof *k_region, 1U,
                                 "Region", "") != 0) {
        return false;
    }
    mesh_map_stack_init(&fixture->stack);
    /* The region first, so a win for the world base is the rule and not the order. */
    return mesh_map_stack_add(&fixture->stack, fixture->region.path) == 0 &&
           mesh_map_stack_add(&fixture->stack, fixture->world.path) == 0;
}

static void stack_fixture_close(struct stack_fixture *fixture) {
    mesh_map_stack_close(&fixture->stack);
    mesh_test_map_pack_remove(&fixture->world);
    mesh_test_map_pack_remove(&fixture->region);
}

static bool stack_picks(const struct mesh_map_stack *stack, struct mesh_map_tile_key key,
                        size_t pack, struct mesh_map_tile_key from) {
    struct mesh_map_stack_pick pick;
    return mesh_map_stack_resolve(stack, key, &pick) && pick.pack == pack &&
           pick.key.zoom == from.zoom && pick.key.x == from.x && pick.key.y == from.y;
}

enum { STACK_REGION = 0, STACK_WORLD = 1 };

/*
 * The deepest answer wins, and at the same depth the pack that stops sooner does.
 *
 * The second half is the one that is easy to get backwards. At zoom 2 the region holds a tile
 * too, but it was drawn from the region alone and the rest of that quarter of the Earth is blank
 * on it; the world base's tile of the same place is the whole picture.
 */
MESH_TEST_CASE(map_stack_draws_each_tile_from_the_pack_that_knows_it_best, unit) {
    struct stack_fixture fixture;
    MESH_TEST_FAIL_IF_CLEANUP(!stack_fixture_open(&fixture), stack_fixture_close(&fixture),
                              "two packs written and stacked");
    const struct mesh_map_stack *const stack = &fixture.stack;

    MESH_TEST_FAIL_IF_CLEANUP(!stack_picks(stack, (struct mesh_map_tile_key){2, 0, 1}, STACK_WORLD,
                                           (struct mesh_map_tile_key){2, 0, 1}),
                              stack_fixture_close(&fixture),
                              "both hold it: the world base, which stops sooner");
    MESH_TEST_FAIL_IF_CLEANUP(!stack_picks(stack, (struct mesh_map_tile_key){3, 1, 2}, STACK_REGION,
                                           (struct mesh_map_tile_key){3, 1, 2}),
                              stack_fixture_close(&fixture),
                              "a region's own tile beats the base enlarged");
    MESH_TEST_FAIL_IF_CLEANUP(!stack_picks(stack, (struct mesh_map_tile_key){3, 0, 0}, STACK_WORLD,
                                           (struct mesh_map_tile_key){2, 0, 0}),
                              stack_fixture_close(&fixture),
                              "outside the region, the base enlarged");
    MESH_TEST_FAIL_IF_CLEANUP(!stack_picks(stack, (struct mesh_map_tile_key){6, 9, 17},
                                           STACK_REGION, (struct mesh_map_tile_key){4, 2, 4}),
                              stack_fixture_close(&fixture),
                              "past the region, the region enlarged");
    MESH_TEST_FAIL_IF_CLEANUP(!stack_picks(stack, (struct mesh_map_tile_key){8, 32, 64},
                                           STACK_REGION, (struct mesh_map_tile_key){4, 2, 4}),
                              stack_fixture_close(&fixture), "as far as the overzoom reaches");

    struct mesh_map_stack_pick pick;
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_map_stack_resolve(stack, (struct mesh_map_tile_key){9, 64, 128}, &pick),
        stack_fixture_close(&fixture), "and no further");
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_map_stack_resolve(stack, (struct mesh_map_tile_key){7, 0, 0}, &pick),
        stack_fixture_close(&fixture), "the base is not enlarged past its reach either");

    uint8_t *const bytes = malloc(MESH_MAP_TILE_BYTES_MAX);
    MESH_TEST_FAIL_IF_CLEANUP(bytes == NULL, stack_fixture_close(&fixture), "a read buffer");
    const bool picked = mesh_map_stack_resolve(stack, (struct mesh_map_tile_key){6, 9, 17}, &pick);
    const int length =
        picked ? mesh_map_stack_read(&fixture.stack, &pick, bytes, MESH_MAP_TILE_BYTES_MAX) : -1;
    struct mesh_map_stack_pick nowhere = {.pack = 2U, .key = {0, 0, 0}};
    const int refused =
        mesh_map_stack_read(&fixture.stack, &nowhere, bytes, MESH_MAP_TILE_BYTES_MAX);
    free(bytes);
    MESH_TEST_FAIL_IF_CLEANUP(length <= 0, stack_fixture_close(&fixture),
                              "a pick reads the tile it names");
    MESH_TEST_FAIL_IF_CLEANUP(refused != -EINVAL, stack_fixture_close(&fixture),
                              "and a pick of no pack reads nothing");
    MESH_TEST_FAIL_IF_CLEANUP(
        strcmp(mesh_map_stack_attribution(stack), "(c) OpenStreetMap contributors") != 0,
        stack_fixture_close(&fixture), "the credit is the first pack's that has one");
    stack_fixture_close(&fixture);
    record_success(test_name);
}

/* Two regions meeting at a border, same depth, same reach: the one added first, every time. */
MESH_TEST_CASE(map_stack_breaks_a_tie_by_order, unit) {
    struct mesh_test_map_pack first;
    struct mesh_test_map_pack second;
    memset(&first, 0, sizeof first);
    memset(&second, 0, sizeof second);
    const size_t count = sizeof k_region / sizeof *k_region;
    struct mesh_map_stack stack;
    mesh_map_stack_init(&stack);
    const bool made = mesh_test_map_pack_write(&first, k_region, count, 0U, "A", "") == 0 &&
                      mesh_test_map_pack_write(&second, k_region, count, 1U, "B", "") == 0 &&
                      mesh_map_stack_add(&stack, first.path) == 0 &&
                      mesh_map_stack_add(&stack, second.path) == 0;
    const bool first_wins = made && stack_picks(&stack, (struct mesh_map_tile_key){4, 2, 4}, 0U,
                                                (struct mesh_map_tile_key){4, 2, 4});
    mesh_map_stack_close(&stack);
    mesh_test_map_pack_remove(&first);
    mesh_test_map_pack_remove(&second);
    MESH_TEST_FAIL_IF(!made, "two packs written and stacked");
    MESH_TEST_FAIL_IF(!first_wins, "the first pack draws");
    record_success(test_name);
}

/* Moves a written pack into `dir` under `name`; false when it could not. */
static bool stack_place(struct mesh_test_map_pack *pack, const char *dir, const char *name,
                        char *out, size_t out_len) {
    snprintf(out, out_len, "%s/%s", dir, name);
    if (rename(pack->path, out) != 0) {
        return false;
    }
    pack->path[0] = '\0';
    return true;
}

static bool stack_write_junk(const char *dir, const char *name, char *out, size_t out_len) {
    snprintf(out, out_len, "%s/%s", dir, name);
    FILE *const file = fopen(out, "wb");
    if (file == NULL) {
        return false;
    }
    (void)fputs("not a pack", file);
    return fclose(file) == 0;
}

/*
 * A maps directory is its `.mctp` files in name order, and nothing else in it.
 *
 * What else is in it is what a real card has: a download still arriving (`.part`), the Finder's
 * AppleDouble shadow of a file copied from a Mac (`._name.mctp`), and a pack that is damaged.
 * The first two are not packs by name and the third is skipped with the rest still drawn; a name
 * in capitals is a FAT32 card written somewhere else and is a pack like any other, and sorts
 * with the rest ignoring case - `Bravo` after `alpha`, where a byte order would put it first.
 */
MESH_TEST_CASE(map_stack_reads_a_maps_directory, unit) {
    char dir[] = "/tmp/meshclient_maps_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "a scratch directory");

    const size_t count = sizeof k_region / sizeof *k_region;
    struct mesh_test_map_pack a;
    struct mesh_test_map_pack b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    char paths[6][128];
    memset(paths, 0, sizeof paths);
    bool made = mesh_test_map_pack_write(&b, k_region, count, 0U, "Bravo", "") == 0 &&
                mesh_test_map_pack_write(&a, k_region, count, 1U, "Alpha", "") == 0;
    made = made && stack_place(&b, dir, "Bravo.mctp", paths[0], sizeof paths[0]) &&
           stack_place(&a, dir, "alpha.MCTP", paths[1], sizeof paths[1]) &&
           stack_write_junk(dir, "charlie.mctp.part", paths[2], sizeof paths[2]) &&
           stack_write_junk(dir, "._alpha.mctp", paths[3], sizeof paths[3]) &&
           stack_write_junk(dir, "damaged.mctp", paths[4], sizeof paths[4]) &&
           stack_write_junk(dir, "notes.txt", paths[5], sizeof paths[5]);

    struct mesh_map_stack stack;
    mesh_map_stack_init(&stack);
    const size_t added = made ? mesh_map_stack_add_dir(&stack, dir) : 0U;
    const bool ordered = stack.count == 2U && strcmp(stack.packs[0].info.name, "Alpha") == 0 &&
                         strcmp(stack.packs[1].info.name, "Bravo") == 0;
    mesh_map_stack_close(&stack);

    char missing[160];
    snprintf(missing, sizeof missing, "%s/nothing-here", dir);
    const size_t none = mesh_map_stack_add_dir(&stack, missing);

    for (size_t i = 0U; i < 6U; ++i) {
        if (paths[i][0] != '\0') {
            unlink(paths[i]);
        }
    }
    mesh_test_map_pack_remove(&a);
    mesh_test_map_pack_remove(&b);
    rmdir(dir);

    MESH_TEST_FAIL_IF(!made, "the directory is laid out");
    MESH_TEST_FAIL_IF(added != 2U, "the two packs open and nothing else does");
    MESH_TEST_FAIL_IF(!ordered, "in name order, whatever the case");
    MESH_TEST_FAIL_IF(none != 0U || stack.count != 0U, "a directory that is not there opens none");
    record_success(test_name);
}

/*
 * A directory with more candidates than are looked at keeps the first ones *by name*.
 *
 * Seventy damaged files between two real packs: `a.mctp` sorts first and is kept, `c.mctp` is
 * past the names considered and is not, whichever order the filesystem lists them in. Taking the
 * first names listed instead would make which packs open depend on the order the card was
 * written in.
 */
MESH_TEST_CASE(map_stack_keeps_the_first_names_not_the_first_listed, unit) {
    char dir[] = "/tmp/meshclient_maps_XXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "a scratch directory");

    enum { JUNK = 70 };
    char paths[JUNK + 2][128];
    memset(paths, 0, sizeof paths);
    const size_t count = sizeof k_region / sizeof *k_region;
    struct mesh_test_map_pack first;
    struct mesh_test_map_pack last;
    memset(&first, 0, sizeof first);
    memset(&last, 0, sizeof last);
    bool made = mesh_test_map_pack_write(&last, k_region, count, 0U, "Last", "") == 0 &&
                stack_place(&last, dir, "c.mctp", paths[JUNK], sizeof paths[JUNK]);
    for (int i = 0; made && i < JUNK; ++i) {
        char name[32];
        snprintf(name, sizeof name, "b%02d.mctp", i);
        made = stack_write_junk(dir, name, paths[i], sizeof paths[i]);
    }
    made = made && mesh_test_map_pack_write(&first, k_region, count, 1U, "First", "") == 0 &&
           stack_place(&first, dir, "a.mctp", paths[JUNK + 1], sizeof paths[JUNK + 1]);

    struct mesh_map_stack stack;
    mesh_map_stack_init(&stack);
    const size_t added = made ? mesh_map_stack_add_dir(&stack, dir) : 0U;
    const bool kept_first = stack.count == 1U && strcmp(stack.packs[0].info.name, "First") == 0;
    mesh_map_stack_close(&stack);

    for (size_t i = 0U; i < (size_t)JUNK + 2U; ++i) {
        if (paths[i][0] != '\0') {
            unlink(paths[i]);
        }
    }
    mesh_test_map_pack_remove(&first);
    mesh_test_map_pack_remove(&last);
    rmdir(dir);

    MESH_TEST_FAIL_IF(!made, "the directory is laid out");
    MESH_TEST_FAIL_IF(added != 1U || !kept_first, "the pack that sorts first opens, and only it");
    record_success(test_name);
}
