/*
 * The one translation unit that holds Wuffs' code. See third_party/wuffs-config/mesh_wuffs.h for
 * what is in it and what deliberately is not; the two decoders the client calls are
 * src/map/tile_image.c and src/utils/inflate.c.
 */

#define WUFFS_IMPLEMENTATION
#include "mesh_wuffs.h"
