#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pb_bytes_array_s {
    size_t size;
    uint8_t bytes[1];
} pb_bytes_array_t;

typedef struct pb_ostream_s {
    uint8_t *buffer;
    size_t size;
    size_t bytes_written;
    void *state;
} pb_ostream_t;

typedef struct pb_istream_s {
    const uint8_t *buffer;
    size_t bytes_left;
    void *state;
} pb_istream_t;

#define PB_RETURN_ERROR(stream, msg) \
    do { \
        (void)(stream); \
        (void)(msg); \
        return false; \
    } while (0)

#ifdef __cplusplus
}
#endif
