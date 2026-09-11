#ifndef MESH_TILE_BENCH_WUFFS_H
#define MESH_TILE_BENCH_WUFFS_H

/* Wuffs cut down to what decoding a PNG needs, so the size probe measures that decoder rather
 * than the thirty other codecs the release file carries. wuffs_impl.c includes this with
 * WUFFS_IMPLEMENTATION defined; everything else sees declarations only. */
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__ZLIB
#include "wuffs-v0.4.c"

#endif
