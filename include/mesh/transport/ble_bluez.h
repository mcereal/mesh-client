#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;
struct mesh_bluez_client;

/*
 * Meshtastic BLE GATT contract (https://meshtastic.org/docs/development/device/client-api/).
 * Not the Nordic UART Service: ToRadio takes one raw protobuf per write with no length framing,
 * inbound is pull-based (FromNum notifies, the client then reads FromRadio until it returns empty).
 */
#define MESH_BLE_MESHTASTIC_SERVICE_UUID "6BA1B218-15A8-461F-9FA8-5DCAE273EAFD"
#define MESH_BLE_TORADIO_UUID "F75C76D2-129E-4DAD-A1DD-7866124401E7"
#define MESH_BLE_FROMRADIO_UUID "2C55E69E-4993-11ED-B878-0242AC120002"
#define MESH_BLE_FROMNUM_UUID "ED9DA18C-A800-4F66-A670-AA7547E34453"
#define MESH_BLE_LOGRADIO_UUID "5A3D6E49-06E6-4423-9944-E9DE8CDF9547"

/* Largest payload the firmware accepts on ToRadio / returns from FromRadio. */
#define MESH_BLE_MAX_PACKET_SIZE 512U

struct mesh_bluez_meshtastic_chars {
    char toradio_path[128];
    char fromradio_path[128];
    char fromnum_path[128];
    char logradio_path[128]; /* empty if the node does not expose it */
};

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
    /* In-flight Device1.Connect, tracked by reply serial so the event loop never blocks on it. */
    uint32_t connect_serial;
    int connect_state; /* 0 idle, 1 pending, 2 done (see connect_result) */
    int connect_result;
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
    /* Device1.ServicesResolved polls that report false before the mock flips to true
       (0 = resolved on the first poll, i.e. BlueZ already had the GATT database cached). */
    unsigned services_resolved_after_polls;
    int services_resolved_result;
    /* Connect reply polls that stay pending before the mock completes with connect_result. */
    unsigned connect_pending_polls;
    /* Device1.Connected polls that report true before the mock reports the link dropped
       (0 = never drops). */
    unsigned connected_drops_after_polls;
    /* Writes that succeed before the mock starts failing them with write_result_late
       (0 = write_result applies to every call). */
    unsigned write_fail_after_calls;
    int write_result_late;
    const char *toradio_char_path;
    const char *fromradio_char_path;
    const char *fromnum_char_path;
    /* Scripted FromRadio reads: each read returns the next payload, then empty. */
    const uint8_t *const *read_payloads;
    const size_t *read_payload_lengths;
    size_t read_payload_count;
    size_t *read_index;
    int read_result;
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
int mesh_bluez_client_list_meshtastic(struct mesh_bluez_client *client,
                                      struct mesh_bluez_device_info *devices, size_t capacity,
                                      size_t *count);
/* Blocking Device1.Connect (up to the 25 s D-Bus default). Kept for tools; the transport uses
   the begin/poll pair below so the UI and buttons stay live while BlueZ works. */
int mesh_bluez_client_connect(struct mesh_bluez_client *client, const char *device_path);
/* Sends Device1.Connect and returns at once. -EBUSY if one is already in flight. */
int mesh_bluez_client_connect_begin(struct mesh_bluez_client *client, const char *device_path);
/* 1 when the reply has arrived (*out_result 0 or a negative errno), 0 while pending, -EINVAL if
   nothing is in flight. Replies are picked up by mesh_bluez_client_process(). */
int mesh_bluez_client_connect_poll(struct mesh_bluez_client *client, int *out_result);
/* Forget an in-flight Connect (a late reply is then ignored). Safe when none is pending. */
void mesh_bluez_client_connect_cancel(struct mesh_bluez_client *client);
int mesh_bluez_client_disconnect(struct mesh_bluez_client *client, const char *device_path);
/* Device1.ServicesResolved. BlueZ's Connect returns once the link is up, but the GATT
   characteristics only appear on the bus after service discovery, which can take several
   seconds when nothing is cached. Callers poll this before looking them up. */
/* Device1.Connected. False once BlueZ has seen the radio drop the link. */
int mesh_bluez_client_device_connected(struct mesh_bluez_client *client, const char *device_path,
                                       bool *out_connected);
int mesh_bluez_client_services_resolved(struct mesh_bluez_client *client, const char *device_path,
                                        bool *out_resolved);
int mesh_bluez_client_subscribe(struct mesh_bluez_client *client, const char *device_path,
                                const char *char_uuid);
int mesh_bluez_client_write(struct mesh_bluez_client *client, const char *device_path,
                            const char *char_uuid, const uint8_t *data, size_t len);
int mesh_bluez_client_read(struct mesh_bluez_client *client, const char *char_path, uint8_t *out,
                           size_t capacity, size_t *out_len);
int mesh_bluez_client_find_meshtastic_characteristics(struct mesh_bluez_client *client,
                                                      const char *device_path,
                                                      struct mesh_bluez_meshtastic_chars *out);
int mesh_bluez_client_attach_loop(struct mesh_bluez_client *client, struct mesh_event_loop *loop);
void mesh_bluez_client_detach_loop(struct mesh_bluez_client *client);
int mesh_bluez_client_process(struct mesh_bluez_client *client);
void mesh_bluez_client_set_notification_handler(struct mesh_bluez_client *client,
                                                mesh_bluez_notification_callback callback,
                                                void *userdata);

void mesh_bluez_client_mock_enable(const struct mesh_bluez_mock_config *config);
void mesh_bluez_client_mock_disable(void);
void mesh_bluez_client_mock_emit_notification(const char *char_path, const uint8_t *data,
                                              size_t len);

#ifdef __cplusplus
}
#endif
