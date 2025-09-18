#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_PROTO_MAX_FRAME_SIZE 512

int mesh_proto_varint_encode(uint32_t value, uint8_t *out, size_t out_len, size_t *written);
int mesh_proto_varint_decode(const uint8_t *buffer, size_t buffer_len, uint32_t *value, size_t *consumed);

int mesh_proto_frame_encode(const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_len,
                            size_t *written);
int mesh_proto_frame_decode(const uint8_t *frame, size_t frame_len, size_t *header_len, size_t *payload_len);

#ifdef __cplusplus
}
#endif
