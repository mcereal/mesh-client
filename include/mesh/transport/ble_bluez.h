#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;
struct mesh_bluez_client;

#ifdef MESH_HAVE_DBUS
struct DBusWatch;

struct mesh_bluez_watch_entry {
    struct DBusWatch *watch;
    int fd;
    uint32_t events;
    bool registered;
    struct mesh_bluez_client *client;
};
#endif

typedef void (*mesh_bluez_notification_callback)(const uint8_t *data, size_t len, void *userdata);

struct mesh_bluez_client {
    void *connection;
    bool connected;
    struct mesh_event_loop *loop;
    mesh_bluez_notification_callback notification_callback;
    void *notification_userdata;
    char notify_characteristic_path[128];
#ifdef MESH_HAVE_DBUS
    struct mesh_bluez_watch_entry watches[8];
#endif
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
    uint8_t *write_capture_buffer;
    size_t write_capture_capacity;
    size_t *write_capture_length;
    char *write_capture_path;
    size_t write_capture_path_capacity;
    size_t *write_call_count;
    size_t *write_lengths;
    size_t write_lengths_capacity;
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
int mesh_bluez_client_attach_loop(struct mesh_bluez_client *client, struct mesh_event_loop *loop);
void mesh_bluez_client_detach_loop(struct mesh_bluez_client *client);
int mesh_bluez_client_process(struct mesh_bluez_client *client);
void mesh_bluez_client_set_notification_handler(struct mesh_bluez_client *client,
                                                mesh_bluez_notification_callback callback, void *userdata);

void mesh_bluez_client_mock_enable(const struct mesh_bluez_mock_config *config);
void mesh_bluez_client_mock_disable(void);
void mesh_bluez_client_mock_emit_notification(const char *char_path, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
#define MESH_BLE_NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define MESH_BLE_NUS_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define MESH_BLE_NUS_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
