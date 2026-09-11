#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reads a whole file into a fresh buffer the caller frees. NULL when the file is missing,
 * unreadable, empty, or longer than `max_len` - which is a refusal rather than a truncation,
 * because both callers hand the bytes to a radio, and half an image is the one outcome worth a
 * check to avoid.
 */
uint8_t *mesh_file_read(const char *path, size_t max_len, size_t *out_len);

#ifdef __cplusplus
}
#endif
