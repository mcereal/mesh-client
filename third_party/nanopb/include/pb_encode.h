#pragma once

#include "pb.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline pb_ostream_t pb_ostream_from_buffer(uint8_t *buffer, size_t size) {
    pb_ostream_t stream;
    stream.buffer = buffer;
    stream.size = size;
    stream.bytes_written = 0;
    stream.state = NULL;
    return stream;
}

bool pb_encode(pb_ostream_t *stream, const void *fields, const void *src_struct);

#ifdef __cplusplus
}
#endif
