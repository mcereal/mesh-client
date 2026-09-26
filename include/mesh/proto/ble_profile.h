#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A protocol's GATT contract, as a table the BLE link is handed rather than a profile it is
 * written against.
 *
 * Every mesh firmware that talks to a phone over BLE does it the same way in outline: one
 * service to scan for, one characteristic the app writes a frame to, one it subscribes to.
 * Where they differ is the UUIDs and what a notification means. So the link holds one of these
 * and asks it, and a second protocol's BLE is a second table here rather than a second BLE
 * transport - exactly as stream_framing.h is for serial and TCP.
 *
 * Which profile a link uses is the protocol's to say (mesh/core/protocol.h). Which profile a
 * radio *advertises* is the scan's to say: it looks for every profile it knows and tags each
 * radio with the one it found.
 */

/* What a notification on the subscribed characteristic means. */
enum mesh_ble_inbound {
    /* A doorbell. The value is only a counter; the frames are read from `read_uuid`, one per
       read, until a read comes back empty. Meshtastic's FromNum/FromRadio. */
    MESH_BLE_INBOUND_PULL = 0,
    /* The value is the frame. One notification, one frame, nothing to read. The Nordic UART
       shape MeshCore's companion firmware uses. */
    MESH_BLE_INBOUND_NOTIFY,
};

struct mesh_ble_profile {
    /* For log lines and tests. A literal. */
    const char *name;
    /* What the radio advertises, and what the scan filters on. */
    const char *service_uuid;
    /* App to radio: one frame per write, no length prefix. */
    const char *write_uuid;
    /* Subscribed on connect. What its value means is `inbound`. */
    const char *notify_uuid;
    /* PULL only: read until empty after each notification. NULL for NOTIFY. */
    const char *read_uuid;
    /* Optional: a characteristic the link looks up but does not need. NULL when there is none. */
    const char *log_uuid;
    enum mesh_ble_inbound inbound;
    /* The largest frame either way. It must be at most MESH_BLE_MAX_PACKET_SIZE, since that is
       what the link's queue and read buffer hold; a larger one is refused on connect. */
    size_t max_frame;
    /*
     * Also list a *bonded* radio whose record shows only an nRF52 DFU bootloader's service.
     * BlueZ leaves a radio that way after a DFU install, until something connects and looks
     * again (docs/transport.md). Only one profile should claim those, because the record says
     * nothing about which firmware came back; Meshtastic's does, since it is the one this
     * client installs.
     */
    bool adopts_bonded_dfu;
};

/* Meshtastic's GATT contract (https://meshtastic.org/docs/development/device/client-api/). */
#define MESH_BLE_MESHTASTIC_SERVICE_UUID "6BA1B218-15A8-461F-9FA8-5DCAE273EAFD"
#define MESH_BLE_TORADIO_UUID "F75C76D2-129E-4DAD-A1DD-7866124401E7"
#define MESH_BLE_FROMRADIO_UUID "2C55E69E-4993-11ED-B878-0242AC120002"
#define MESH_BLE_FROMNUM_UUID "ED9DA18C-A800-4F66-A670-AA7547E34453"
#define MESH_BLE_LOGRADIO_UUID "5A3D6E49-06E6-4423-9944-E9DE8CDF9547"

/* MeshCore's companion radio: the Nordic UART Service. The app writes RX and subscribes to TX,
   whose notifications are the frames themselves (examples/companion_radio, SerialBLEInterface). */
#define MESH_BLE_NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define MESH_BLE_NUS_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define MESH_BLE_NUS_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

/* The most any profile may carry in one frame - ATT's own limit, and what Meshtastic uses. */
#define MESH_BLE_MAX_PACKET_SIZE 512U

/* ToRadio, FromNum then FromRadio until empty, LogRadio when the firmware has it. */
extern const struct mesh_ble_profile mesh_ble_profile_meshtastic;

/* RX, TX notified with one frame each; MAX_FRAME_SIZE (176) either way. */
extern const struct mesh_ble_profile mesh_ble_profile_meshcore;

/* Every profile the scan looks for, in the order a radio advertising several is tagged. */
extern const struct mesh_ble_profile *const mesh_ble_known_profiles[];
extern const size_t mesh_ble_known_profile_count;

/* True when a link can carry `profile`: the four UUIDs its inbound model needs are there, and
   its largest frame fits. */
bool mesh_ble_profile_usable(const struct mesh_ble_profile *profile);

#ifdef __cplusplus
}
#endif
