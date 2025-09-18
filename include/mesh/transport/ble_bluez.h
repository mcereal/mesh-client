#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_bluez_client {
    void *connection;
    bool connected;
};

struct mesh_bluez_device_info {
    char address[32];
    char name[64];
    int16_t rssi;
};

struct mesh_bluez_mock_config {
    int init_result;
    int check_ready_result;
    int find_adapter_result;
    const char *adapter_path;
    int start_discovery_result;
    int stop_discovery_result;
    int connect_result;
    int disconnect_result;
    int write_result;
    int subscribe_result;
    const char *rx_char_path;
    const char *tx_char_path;
    const struct mesh_bluez_device_info *devices;
    size_t device_count;
    int list_result;
};

int mesh_bluez_client_init(struct mesh_bluez_client *client);
void mesh_bluez_client_shutdown(struct mesh_bluez_client *client);
int mesh_bluez_client_check_ready(struct mesh_bluez_client *client);
int mesh_bluez_client_find_adapter(struct mesh_bluez_client *client, char *path, size_t path_len);
int mesh_bluez_client_start_discovery(struct mesh_bluez_client *client, const char *adapter_path);
int mesh_bluez_client_stop_discovery(struct mesh_bluez_client *client, const char *adapter_path);
int mesh_bluez_client_list_meshtastic(struct mesh_bluez_client *client, struct mesh_bluez_device_info *devices,
                                      size_t capacity, size_t *count);
int mesh_bluez_client_connect(struct mesh_bluez_client *client, const char *device_path);
int mesh_bluez_client_disconnect(struct mesh_bluez_client *client, const char *device_path);
int mesh_bluez_client_subscribe(struct mesh_bluez_client *client, const char *device_path, const char *char_uuid);
int mesh_bluez_client_write(struct mesh_bluez_client *client, const char *device_path, const char *char_uuid,
                            const uint8_t *data, size_t len);
int mesh_bluez_client_find_nus_characteristics(struct mesh_bluez_client *client, const char *device_path,
                                               char *rx_path, size_t rx_len, char *tx_path, size_t tx_len);

void mesh_bluez_client_mock_enable(const struct mesh_bluez_mock_config *config);
void mesh_bluez_client_mock_disable(void);

#ifdef __cplusplus
}
#endif
#define MESH_BLE_NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define MESH_BLE_NUS_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define MESH_BLE_NUS_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
