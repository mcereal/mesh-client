#include "mesh/map/stack.h"

#include "inkwell/base/file.h"
#include "inkwell/base/log.h"

#include "mesh/map/tile_image.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STACK_SUFFIX ".mctp"

/* How many names a directory is read into before it is sorted. More than the stack holds, so a
   directory with a few broken files in it still fills the stack from the rest. */
#define STACK_DIR_NAMES_MAX (MESH_MAP_STACK_PACKS_MAX * 4U)
#define STACK_NAME_MAX 256U

void mesh_map_stack_init(struct mesh_map_stack *stack) {
    if (stack != NULL) {
        memset(stack, 0, sizeof *stack);
    }
}

int mesh_map_stack_add(struct mesh_map_stack *stack, const char *path) {
    if (stack == NULL || path == NULL || path[0] == '\0') {
        return -EINVAL;
    }
    if (stack->count >= MESH_MAP_STACK_PACKS_MAX) {
        return -ENOSPC;
    }
    const int opened = mesh_map_source_open_pack(path, &stack->packs[stack->count]);
    if (opened < 0) {
        return opened;
    }
    ++stack->count;
    return 0;
}

struct stack_names {
    char names[STACK_DIR_NAMES_MAX][STACK_NAME_MAX];
    size_t count;
};

static bool stack_is_pack_name(const char *name) {
    const size_t length = strlen(name);
    const size_t suffix = sizeof STACK_SUFFIX - 1U;
    /* A leading dot is macOS's AppleDouble shadow of a file copied onto FAT, `._world.mctp`,
       which is a few hundred bytes of Finder metadata wearing the pack's name. */
    return name[0] != '.' && length > suffix && length < STACK_NAME_MAX &&
           strcasecmp(name + length - suffix, STACK_SUFFIX) == 0;
}

static void stack_collect(void *context, const char *name) {
    struct stack_names *const names = context;
    if (names->count < STACK_DIR_NAMES_MAX && stack_is_pack_name(name)) {
        memcpy(names->names[names->count], name, strlen(name) + 1U);
        ++names->count;
    }
}

static int stack_compare_names(const void *a, const void *b) { return strcmp(a, b); }

size_t mesh_map_stack_add_dir(struct mesh_map_stack *stack, const char *dir) {
    if (stack == NULL || dir == NULL || !inkwell_file_is_dir(dir)) {
        return 0U;
    }
    /* On the heap: sixty-four names of a path's length is 16 KiB, which is not a thing to put
       on the stack of whatever frame asked for a map. */
    struct stack_names *const names = calloc(1U, sizeof *names);
    if (names == NULL) {
        return 0U;
    }
    const int listed = inkwell_file_list(dir, stack_collect, names);
    if (listed < 0) {
        inkwell_log_warn("map", "Map packs in %s could not be listed: %s", dir, strerror(-listed));
    }
    qsort(names->names, names->count, sizeof names->names[0], stack_compare_names);

    size_t added = 0U;
    for (size_t i = 0U; i < names->count; ++i) {
        char path[1024];
        if (snprintf(path, sizeof path, "%s/%s", dir, names->names[i]) >= (int)sizeof path) {
            continue;
        }
        const int opened = mesh_map_stack_add(stack, path);
        if (opened == -ENOSPC) {
            inkwell_log_warn("map", "Map packs past the first %u in %s are not drawn",
                             (unsigned)MESH_MAP_STACK_PACKS_MAX, dir);
            break;
        }
        if (opened < 0) {
            inkwell_log_warn("map", "Map pack %s could not be opened: %s", path, strerror(-opened));
            continue;
        }
        ++added;
    }
    free(names);
    return added;
}

/* The tile `pack` would read to draw `key`, or false when it has none. */
static bool stack_answer(const struct mesh_map_source *pack, struct mesh_map_tile_key key,
                         struct mesh_map_tile_key *out) {
    const uint8_t deepest = pack->info.max_zoom;
    if (key.zoom <= deepest) {
        *out = key;
        return mesh_map_source_has(pack, key);
    }
    if ((unsigned)(key.zoom - deepest) > MESH_MAP_TILE_OVERZOOM_LEVELS ||
        !mesh_map_tile_ancestor(key, deepest, out)) {
        return false;
    }
    return mesh_map_source_has(pack, *out);
}

bool mesh_map_stack_resolve(const struct mesh_map_stack *stack, struct mesh_map_tile_key key,
                            struct mesh_map_stack_pick *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (stack == NULL || out == NULL || !mesh_map_tile_key_valid(key)) {
        return false;
    }
    bool found = false;
    for (size_t i = 0U; i < stack->count; ++i) {
        struct mesh_map_tile_key from;
        if (!stack_answer(&stack->packs[i], key, &from)) {
            continue;
        }
        /* Deeper wins; at the same depth the pack that stops sooner wins; after that the
           earlier one, which is what the strict comparisons leave in place. */
        const bool better = !found || from.zoom > out->key.zoom ||
                            (from.zoom == out->key.zoom &&
                             stack->packs[i].info.max_zoom < stack->packs[out->pack].info.max_zoom);
        if (better) {
            out->pack = i;
            out->key = from;
            found = true;
        }
    }
    return found;
}

int mesh_map_stack_read(struct mesh_map_stack *stack, const struct mesh_map_stack_pick *pick,
                        uint8_t *out, size_t cap) {
    if (stack == NULL || pick == NULL || pick->pack >= stack->count) {
        return -EINVAL;
    }
    return mesh_map_source_read(&stack->packs[pick->pack], pick->key, out, cap);
}

const char *mesh_map_stack_attribution(const struct mesh_map_stack *stack) {
    if (stack != NULL) {
        for (size_t i = 0U; i < stack->count; ++i) {
            if (stack->packs[i].info.attribution[0] != '\0') {
                return stack->packs[i].info.attribution;
            }
        }
    }
    return "";
}

void mesh_map_stack_close(struct mesh_map_stack *stack) {
    if (stack == NULL) {
        return;
    }
    for (size_t i = 0U; i < stack->count; ++i) {
        mesh_map_source_close(&stack->packs[i]);
    }
    memset(stack, 0, sizeof *stack);
}
