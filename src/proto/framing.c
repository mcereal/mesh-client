#include "mesh/proto/framing.h"

#include <errno.h>
#include <stdbool.h>

int mesh_proto_varint_encode(uint32_t value, uint8_t *out, size_t out_len, size_t *written) {
    if (out == NULL || written == NULL) {
        return -EINVAL;
    }

    size_t index = 0;
    while (true) {
        if (index >= out_len) {
            return -ENOSPC;
        }
        uint8_t byte = (uint8_t)(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) {
            byte |= 0x80U;
        }
        out[index++] = byte;
        if (value == 0U) {
            break;
        }
    }
    *written = index;
    return 0;
}

int mesh_proto_varint_decode(const uint8_t *buffer, size_t buffer_len, uint32_t *value, size_t *consumed) {
    if (buffer == NULL || value == NULL || consumed == NULL) {
        return -EINVAL;
    }

    uint32_t result = 0;
    size_t shift = 0;
    size_t index = 0;

    while (index < buffer_len && shift < 32U) {
        uint8_t byte = buffer[index++];
        result |= ((uint32_t)(byte & 0x7FU)) << shift;
        if ((byte & 0x80U) == 0U) {
            *value = result;
            *consumed = index;
            return 0;
        }
        shift += 7U;
    }

    return -EINVAL;
}

int mesh_proto_frame_encode(const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_len,
                            size_t *written) {
    if ((payload_len > 0U && payload == NULL) || out == NULL || written == NULL) {
        return -EINVAL;
    }

    size_t header_written = 0;
    int varint_result = mesh_proto_varint_encode((uint32_t)payload_len, out, out_len, &header_written);
    if (varint_result < 0) {
        return varint_result;
    }

    if (header_written + payload_len > out_len) {
        return -ENOSPC;
    }

    if (payload_len > 0U) {
        for (size_t i = 0; i < payload_len; ++i) {
            out[header_written + i] = payload[i];
        }
    }

    *written = header_written + payload_len;
    return 0;
}

int mesh_proto_frame_decode(const uint8_t *frame, size_t frame_len, size_t *header_len, size_t *payload_len) {
    if (frame == NULL || header_len == NULL || payload_len == NULL) {
        return -EINVAL;
    }

    uint32_t decoded_length = 0;
    size_t consumed = 0;
    int result = mesh_proto_varint_decode(frame, frame_len, &decoded_length, &consumed);
    if (result < 0) {
        return result;
    }

    if (consumed + decoded_length > frame_len) {
        return -EINVAL;
    }

    *header_len = consumed;
    *payload_len = decoded_length;
    return 0;
}
