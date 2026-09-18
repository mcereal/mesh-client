#pragma once

/*
 * A raw deflate stream (RFC 1951) into a buffer, and the CRC32 a zip checks the result with.
 *
 * What a zip member is: bare deflate with no zlib or gzip framing, and the length and the CRC it
 * must come out to kept elsewhere, in the central directory. So this takes no framing and checks
 * nothing on its own - it answers how many bytes came out, and mesh_crc32() answers whether they
 * are the right ones. Both halves are Wuffs (third_party/wuffs-config/mesh_wuffs.h), which the
 * map's PNG decoder already compiles in.
 *
 * Whole buffers rather than a stream, because the one caller has the member in memory and knows
 * the size it must produce: a firmware image is a megabyte or two, and the destination is
 * allocated at exactly the length the directory promised.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_inflate_result {
    MESH_INFLATE_OK = 0,
    /* Not a deflate stream, or one that stopped before its final block. */
    MESH_INFLATE_CORRUPT,
    /* The stream is longer than `out_cap`. Told apart from CORRUPT because it is the one answer
       where every byte written is still correct - there are just more of them. */
    MESH_INFLATE_TOO_LONG,
    MESH_INFLATE_NO_MEMORY,
};

/*
 * Inflates `in` into `out`, which holds `out_cap` bytes, and sets `*out_len` to how many were
 * written - on every result, not only OK. Bytes after the end of the stream are ignored.
 *
 * A result of OK says the stream was well formed and fitted, and nothing about whether it is the
 * stream that was wanted: that is a length and a CRC, and they are the caller's to check.
 */
enum mesh_inflate_result mesh_inflate(const uint8_t *in, size_t in_len, uint8_t *out,
                                      size_t out_cap, size_t *out_len);

/* The IEEE CRC32 - zip's, gzip's and PNG's - of `len` bytes, into `*out`. False only when the
   hasher could not be allocated: 0 is a CRC real data can have, so it cannot be the refusal. */
bool mesh_crc32(const uint8_t *bytes, size_t len, uint32_t *out);

#ifdef __cplusplus
}
#endif
