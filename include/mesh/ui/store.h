#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_UI_MAX_DEVICES 16U
#define MESH_UI_MAX_HANDSHAKE_NODES 16U

enum mesh_ui_update_flag {
    MESH_UI_UPDATE_NONE = 0U,
    MESH_UI_UPDATE_DISCOVERY = 1U << 0,
    MESH_UI_UPDATE_HANDSHAKE = 1U << 1,
};
typedef uint32_t mesh_ui_update_flags;

struct mesh_ui_device {
    char identifier[64];
    char name[64];
    int8_t rssi;
    bool connected;
};

struct mesh_ui_node_summary {
    uint32_t node_id;
    char long_name[40];
    char short_name[5];
    uint32_t last_heard;
    float snr;
    bool via_mqtt;
    bool has_hops_away;
    uint8_t hops_away;
};

struct mesh_ui_my_info {
    uint32_t node_num;
    uint32_t nodedb_entries;
    uint32_t reboot_count;
};

struct mesh_ui_handshake_state {
    bool request_in_flight;
    uint32_t request_id;
    bool config_complete;
    uint32_t config_complete_id;
    bool has_my_info;
    struct mesh_ui_my_info my_info;
    bool has_config;
    uint32_t node_count;
    char primary_channel[33];
    char my_short_name[6];
    struct mesh_ui_node_summary nodes[MESH_UI_MAX_HANDSHAKE_NODES];
};

struct mesh_ui_snapshot {
    struct mesh_ui_device devices[MESH_UI_MAX_DEVICES];
    size_t device_count;
    struct mesh_ui_handshake_state handshake;
    bool handshake_valid;
    mesh_ui_update_flags update_flags;
};

struct mesh_ui_store {
    struct mesh_ui_device devices[MESH_UI_MAX_DEVICES];
    size_t device_count;
    struct mesh_ui_handshake_state handshake;
    bool handshake_valid;
    int event_fd;
    mesh_ui_update_flags pending_flags;
};

int mesh_ui_store_init(struct mesh_ui_store *store);
void mesh_ui_store_shutdown(struct mesh_ui_store *store);
void mesh_ui_store_reset(struct mesh_ui_store *store);

int mesh_ui_store_event_fd(const struct mesh_ui_store *store);

void mesh_ui_store_set_discovery(struct mesh_ui_store *store, const struct mesh_ui_device *devices, size_t count);
void mesh_ui_store_set_handshake(struct mesh_ui_store *store, const struct mesh_ui_handshake_state *handshake);

bool mesh_ui_store_consume_updates(struct mesh_ui_store *store, struct mesh_ui_snapshot *snapshot);

#ifdef __cplusplus
}
#endif
