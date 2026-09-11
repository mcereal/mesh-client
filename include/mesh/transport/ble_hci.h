#pragma once

/*
 * Asking for a BLE connection interval: the one thing the OTA path needs from Bluetooth that
 * BlueZ has no D-Bus call for.
 *
 * Measured on 2026-09-10 (docs/radio-firmware-roadmap.md, "What phase 0 measured on BLE"): the
 * Brick connects at 30 ms and stays there, and a peripheral's own request is judged against
 * `conn_{min,max}_interval` in debugfs, which on this kernel are 40 and 56 - 50 and 70 ms. The
 * OTA loader asks for 15 ms, so the Brick refuses it and the loader is held at 30 ms, where a
 * stop-and-wait transfer of a 2 MB image takes about seventeen minutes. What was accepted, at
 * once and with no reconnect, was an `LE Connection Update` issued by the central on the open
 * handle - and that is a raw HCI command, which is this file.
 *
 * Why not the debugfs window instead: it is system-wide and stays where it is put, and it
 * equally lets a peripheral negotiate a link *down*, which is what happened when it was tried.
 * An update on one handle dies with that handle, so there is nothing to give back: the loader
 * reboots at the end of a transfer and takes the 7.5 ms with it.
 *
 * This needs CAP_NET_RAW, which the client has on the Brick because it runs as root. Anywhere
 * it does not, the request fails and the transfer runs at whatever interval the link has - slow,
 * not broken - so every caller treats a failure here as a log line rather than a stop.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One HCI command packet: the type byte, the opcode, the length and fourteen bytes of
   parameters. */
#define MESH_BLE_HCI_CONN_UPDATE_LEN 18U

/* Connection intervals are counted in 1.25 ms units, and 6 is the floor the spec allows. */
#define MESH_BLE_HCI_INTERVAL_7_5_MS 6U

struct mesh_ble_hci_conn_params {
    uint16_t min_interval;        /* 1.25 ms units */
    uint16_t max_interval;        /* 1.25 ms units */
    uint16_t latency;             /* connection events the peripheral may skip */
    uint16_t supervision_timeout; /* 10 ms units */
};

/* What an OTA asks for: 7.5 ms, no latency, and the loader's own four-second supervision
   timeout. */
extern const struct mesh_ble_hci_conn_params mesh_ble_hci_ota_params;

/* Encodes `LE Connection Update` (OGF 0x08, OCF 0x0013) for `handle` into `out`. Returns the
   packet length, or 0 for parameters the spec does not allow - an interval outside 6..3200, a
   min above the max, or a supervision timeout too short to outlive one latency window. */
size_t mesh_ble_hci_encode_conn_update(uint16_t handle,
                                       const struct mesh_ble_hci_conn_params *params,
                                       uint8_t out[MESH_BLE_HCI_CONN_UPDATE_LEN]);

/* "9C:13:9E:9D:0A:D9" into a bdaddr_t, which is the same six bytes in the opposite order. */
bool mesh_ble_hci_parse_address(const char *text, uint8_t out[6]);

/* "/org/bluez/hci0" -> 0. -EINVAL for anything that does not end in hciN. */
int mesh_ble_hci_adapter_index(const char *adapter_path);

/*
 * Finds the LE connection to `address` on adapter `dev_id` and asks for `params` on it.
 *
 * Returns 0 once the command has been handed to the kernel, -ENOENT when there is no LE link to
 * that address, or -errno from the socket (-EAFNOSUPPORT in a container with no Bluetooth,
 * -EPERM without CAP_NET_RAW). "Handed to the kernel" is all it can promise: the controller
 * answers with a Connection Update Complete that nothing here waits for, and a transfer that
 * gets faster is the evidence it landed.
 */
int mesh_ble_hci_request_interval(int dev_id, const char *address,
                                  const struct mesh_ble_hci_conn_params *params);

#ifdef __cplusplus
}
#endif
