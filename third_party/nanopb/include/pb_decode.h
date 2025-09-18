#pragma once

#include "pb.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline pb_istream_t pb_istream_from_buffer(const uint8_t *buffer, size_t size) {
    pb_istream_t stream;
    stream.buffer = buffer;
    stream.bytes_left = size;
    stream.state = NULL;
    return stream;
}

bool pb_decode(pb_istream_t *stream, const void *fields, void *dest_struct);

#ifdef __cplusplus
}
#endif
