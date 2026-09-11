#ifndef MESH_MAP_WUFFS_PNG_H
#define MESH_MAP_WUFFS_PNG_H

/*
 * Wuffs, cut down to what decoding a PNG needs.
 *
 * The vendored file carries some thirty codecs - JPEG, GIF, BMP, WEBP, CBOR, JSON and the rest -
 * and this is what keeps every one of them out of the binary. WUFFS_CONFIG__MODULES turns the
 * file's default "compile everything" off, and the modules named below are the PNG decoder and
 * exactly its dependencies: zlib for the image data, deflate under that, and the two checksums
 * the two of them verify with.
 *
 * BASE is named by its *sub-modules* rather than whole, and that is worth 31 KiB of the pak.
 * Wuffs splits it seven ways and PNG reaches three: CORE, INTERFACES, and PIXCONV for the
 * swizzler that puts the file's pixels in the order the panel wants. The four left out are
 * FLOATCONV and INTCONV (numbers to and from text, which is what a JSON decoder wants),
 * MAGIC (sniffing a file's type, which a pack has already answered) and UTF8.
 *
 * Naming them individually buys nothing under `--gc-sections`, which drops unreferenced
 * functions anyway - it was worth 128 bytes when measured that way. It is worth 31,264 bytes
 * under plain `-Os`, which is what scripts/cross-build.sh actually uses: with no
 * `-ffunction-sections` there is nothing for a linker to drop *within* an object, so anything
 * compiled into this one ships whether or not it is called. That is a fact about the pak's
 * build rather than about Wuffs, and it is the same fact that makes this decoder cost three
 * times what docs/maps-roadmap.md's measurement predicted - see there.
 *
 * wuffs_png.c includes this with WUFFS_IMPLEMENTATION defined and is the one translation unit
 * that holds the code; everything else that includes it sees declarations only. That is the
 * single-file-library convention, and it is also what lets the implementation be compiled
 * without this project's warning flags while our own code keeps them.
 *
 * Nothing outside src/map includes this. The client's decoder is one function in
 * mesh/map/tile_image.h, which is a seam rather than a wrapper: it names a pixel format, a
 * bounded work buffer and a tile-sized destination, none of which are Wuffs' vocabulary.
 */

#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE__CORE
#define WUFFS_CONFIG__MODULE__BASE__INTERFACES
#define WUFFS_CONFIG__MODULE__BASE__PIXCONV
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__ZLIB

#include "wuffs-v0.4.c"

#endif /* MESH_MAP_WUFFS_PNG_H */
