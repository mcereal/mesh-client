#ifndef MESH_TEST_SUPPORT_MESHCORE_FIXTURE_H
#define MESH_TEST_SUPPORT_MESHCORE_FIXTURE_H

/*
 * A MeshCore companion radio, for the cases that talk to one: the DEVICE_INFO and SELF_INFO a
 * Heltec V3 on companion-v1.17.1 sent over BLE, byte for byte, a fake link that keeps every frame
 * it is handed, and a sync that answers the handshake through to READY.
 *
 * Shared because two suites need the same radio: the conversation's (meshcore.c) and the app's,
 * which restores a backup onto one through a real struct mesh_app.
 */

#include "mesh/core/meshcore.h"
#include "mesh/core/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESH_TEST_MESHCORE_DEVICE_INFO_LEN 82U
#define MESH_TEST_MESHCORE_SELF_INFO_LEN 62U

extern const uint8_t mesh_test_meshcore_device_info[MESH_TEST_MESHCORE_DEVICE_INFO_LEN];
extern const uint8_t mesh_test_meshcore_self_info[MESH_TEST_MESHCORE_SELF_INFO_LEN];

void mesh_test_put_u32(uint8_t *out, uint32_t value);

/* A 148-byte contact record for a key starting with `lead`; `code` is RESP_CONTACT or an
   advert push. The length written. */
size_t mesh_test_meshcore_contact(uint8_t *out, uint8_t code, uint8_t lead, const char *name,
                                  uint8_t type, uint8_t path_len, uint32_t lastmod);

/* A link that keeps what it is handed; `refuse` makes it answer as a full queue would. */
struct mesh_test_meshcore_wire {
    uint8_t frames[32][MESH_MESHCORE_MAX_FRAME];
    size_t lens[32];
    uint32_t ids[32];
    size_t count;
    bool refuse;
};

int mesh_test_meshcore_wire_send(void *ctx, const uint8_t *frame, size_t len, uint32_t frame_id);
/* The command byte of the last frame sent, 0 for none. */
uint8_t mesh_test_meshcore_wire_last(const struct mesh_test_meshcore_wire *wire);

/*
 * Attaches `wire` (cleared) to `protocol` - `meshcore`'s - and answers the handshake to READY: the
 * recorded device and self info with one channel slot, one known contact ("Alice", key 0x40...)
 * and the slot named "Public". False when the conversation asked for something else on the way.
 */
bool mesh_test_meshcore_sync(struct mesh_meshcore *meshcore, struct mesh_protocol *protocol,
                             struct mesh_test_meshcore_wire *wire);

#endif /* MESH_TEST_SUPPORT_MESHCORE_FIXTURE_H */
