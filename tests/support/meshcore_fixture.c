/*
 * A MeshCore companion radio as a Heltec V3 on companion-v1.17.1 introduced itself, and a fake
 * link to drive a conversation with it. See meshcore_fixture.h.
 */

#include "support/meshcore_fixture.h"

#include <errno.h>
#include <string.h>

const uint8_t mesh_test_meshcore_device_info[MESH_TEST_MESHCORE_DEVICE_INFO_LEN] = {
    0x0d, 0x0d, 0xaf, 0x28, 0x1a, 0xa5, 0x09, 0x00, 0x31, 0x34, 0x2d, 0x41, 0x75, 0x67,
    0x2d, 0x32, 0x30, 0x32, 0x36, 0x00, 0x48, 0x65, 0x6c, 0x74, 0x65, 0x63, 0x20, 0x56,
    0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x76, 0x31, 0x2e, 0x31, 0x37, 0x2e, 0x31, 0x2d, 0x64, 0x39,
    0x32, 0x39, 0x36, 0x34, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const uint8_t mesh_test_meshcore_self_info[MESH_TEST_MESHCORE_SELF_INFO_LEN] = {
    0x05, 0x01, 0x16, 0x16, 0xb8, 0xda, 0x09, 0xb0, 0x98, 0xf4, 0xdd, 0x8c, 0x5d, 0xe6, 0x21, 0xf1,
    0xfc, 0x92, 0x3b, 0x02, 0x84, 0xb3, 0xf6, 0xc7, 0x72, 0x7d, 0x41, 0x29, 0xeb, 0x10, 0xa2, 0xb2,
    0xe3, 0xd4, 0xa4, 0x58, 0x2a, 0xd7, 0x17, 0x01, 0xdf, 0x18, 0xfe, 0xfb, 0x00, 0x00, 0x00, 0x00,
    0xbd, 0xe4, 0x0d, 0x00, 0x24, 0xf4, 0x00, 0x00, 0x07, 0x05, 0x4d, 0x50, 0x42, 0x43,
};

void mesh_test_put_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

/* A 148-byte contact record for a key starting with `lead`. */
size_t mesh_test_meshcore_contact(uint8_t *out, uint8_t code, uint8_t lead, const char *name,
                                  uint8_t type, uint8_t path_len, uint32_t lastmod) {
    memset(out, 0, 148U);
    size_t i = 0U;
    out[i++] = code;
    for (size_t k = 0; k < 32U; ++k) {
        out[i + k] = (uint8_t)(lead + k);
    }
    i += 32U;
    out[i++] = type;
    out[i++] = 0U;
    out[i++] = path_len;
    i += 64U;
    memcpy(out + i, name, strlen(name));
    i += 32U;
    mesh_test_put_u32(out + i, lastmod - 60U); /* the sender's own advert stamp */
    i += 4U;
    mesh_test_put_u32(out + i, (uint32_t)(int32_t)37774900);
    i += 4U;
    mesh_test_put_u32(out + i, (uint32_t)(int32_t)-122419400);
    i += 4U;
    mesh_test_put_u32(out + i, lastmod);
    i += 4U;
    return i;
}

int mesh_test_meshcore_wire_send(void *ctx, const uint8_t *frame, size_t len, uint32_t frame_id) {
    struct mesh_test_meshcore_wire *wire = ctx;
    if (wire->refuse) {
        return -EAGAIN;
    }
    if (wire->count >= 32U || len > MESH_MESHCORE_MAX_FRAME) {
        return -ENOBUFS;
    }
    memcpy(wire->frames[wire->count], frame, len);
    wire->lens[wire->count] = len;
    wire->ids[wire->count] = frame_id;
    wire->count += 1U;
    return 0;
}

uint8_t mesh_test_meshcore_wire_last(const struct mesh_test_meshcore_wire *wire) {
    return wire->count > 0U ? wire->frames[wire->count - 1U][0] : 0U;
}

static void feed(const struct mesh_protocol *protocol, const uint8_t *frame, size_t len) {
    mesh_protocol_receive(protocol, frame, len);
}

static void feed_code(const struct mesh_protocol *protocol, uint8_t code) {
    feed(protocol, &code, 1U);
}

bool mesh_test_meshcore_sync(struct mesh_meshcore *meshcore, struct mesh_protocol *protocol,
                             struct mesh_test_meshcore_wire *wire) {
    memset(wire, 0, sizeof *wire);
    mesh_protocol_attach(protocol, mesh_test_meshcore_wire_send, wire);
    if (mesh_protocol_begin(protocol) != 0 ||
        mesh_test_meshcore_wire_last(wire) != MESH_MESHCORE_CMD_DEVICE_QUERY || wire->count != 1U) {
        return false;
    }
    uint8_t device[sizeof mesh_test_meshcore_device_info];
    memcpy(device, mesh_test_meshcore_device_info, sizeof device);
    device[3] = 1U; /* one channel slot, so the walk is short */
    feed(protocol, device, sizeof device);
    if (mesh_test_meshcore_wire_last(wire) != MESH_MESHCORE_CMD_APP_START) {
        return false;
    }
    feed(protocol, mesh_test_meshcore_self_info, sizeof mesh_test_meshcore_self_info);
    if (mesh_test_meshcore_wire_last(wire) == MESH_MESHCORE_CMD_SET_DEVICE_TIME) {
        feed_code(protocol, MESH_MESHCORE_RESP_OK);
    }
    if (mesh_test_meshcore_wire_last(wire) != MESH_MESHCORE_CMD_GET_CONTACTS) {
        return false;
    }
    uint8_t frame[160];
    uint8_t start[5] = {MESH_MESHCORE_RESP_CONTACTS_START, 1, 0, 0, 0};
    feed(protocol, start, sizeof start);
    feed(protocol, frame,
         mesh_test_meshcore_contact(frame, MESH_MESHCORE_RESP_CONTACT, 0x40, "Alice",
                                    MESH_MESHCORE_ADV_CHAT, 2U, 1700000000U));
    uint8_t end[5] = {MESH_MESHCORE_RESP_END_OF_CONTACTS};
    mesh_test_put_u32(end + 1, 1700000000U);
    feed(protocol, end, sizeof end);
    if (mesh_test_meshcore_wire_last(wire) != MESH_MESHCORE_CMD_GET_CHANNEL) {
        return false;
    }
    uint8_t channel[2 + 32 + 16];
    memset(channel, 0, sizeof channel);
    channel[0] = MESH_MESHCORE_RESP_CHANNEL_INFO;
    memcpy(channel + 2, "Public", 6U);
    channel[34] = 0x8b;
    feed(protocol, channel, sizeof channel);
    /* Ready: the message queue is drained, then the battery asked for. */
    if (mesh_test_meshcore_wire_last(wire) != MESH_MESHCORE_CMD_SYNC_NEXT_MESSAGE) {
        return false;
    }
    feed_code(protocol, MESH_MESHCORE_RESP_NO_MORE_MESSAGES);
    if (mesh_test_meshcore_wire_last(wire) != MESH_MESHCORE_CMD_GET_BATT_AND_STORAGE) {
        return false;
    }
    uint8_t battery[11] = {MESH_MESHCORE_RESP_BATT_AND_STORAGE, 0x10, 0x0f};
    feed(protocol, battery, sizeof battery);
    return mesh_meshcore_ready(meshcore);
}
