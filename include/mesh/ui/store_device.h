#pragma once

/*
 * What discovery found, and how to reach it: one row of the Devices tab.
 *
 * The narrowest of the store's records and the one with the fewest readers, which is why it is
 * its own header - `mesh/ui/devices.h` needs a device and nothing else about a snapshot, and
 * a screen that draws a list of radios has no business being rebuilt because a telemetry field
 * moved. See mesh/ui/store.h for the whole.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_UI_MAX_DEVICES 16U
#define MESH_UI_TRANSPORT_STATUS_MAX 32U

/*
 * A network target as the user writes it: "192.168.1.50", "192.168.1.50:4403", or a bracketed
 * v6 literal. MESH_TCP_TARGET_MAX said twice across the seam, exactly as
 * MESH_WAYPOINT_NAME_MAX is: this header names no transport module, and pulling one in to
 * reach one number is how a seam stops being one. network_host_limits_agree_across_the_seam
 * is what holds the two honest.
 */
#define MESH_UI_NETWORK_HOST_MAX 64U

/* How a device is reached. The Devices tab lists every kind in one list, and the app routes
   a connect to the matching transport. */
enum mesh_ui_device_kind {
    MESH_UI_DEVICE_BLE = 0,
    MESH_UI_DEVICE_SERIAL,
    /*
     * A node reached over the network.
     *
     * There is no discovery behind this kind - a network cannot be scanned - so the only row
     * that ever carries it is the link that is already up, named by the address it was
     * configured with. It exists so that row cannot be taken for a Bluetooth bond: BLE is 0,
     * so a slot that never states its kind is a BLE radio by default, and a renderer asking
     * "which icon, which supporting line, may this be forgotten" got three wrong answers from
     * the one field nobody set.
     */
    MESH_UI_DEVICE_TCP,
};

struct mesh_ui_device {
    char identifier[64];
    char name[64];
    int8_t rssi; /* BLE only; 0 for a USB port or a network host, and meaningless unless
                    in_range */
    /* Whether the radio answered the last scan. BlueZ lists every node it holds a bond for,
       so a row can name a radio sitting at home all day; it has no RSSI to show and saying
       "0dBm" about it reads as the strongest signal on the screen. Always true for a USB
       port, which is present or is not a row, and always false for a network host, which
       answers from anywhere and so is evidence of no distance at all - the Status card counts
       this field and means earshot by it. */
    bool in_range;
    bool connected;
    /* BLE only: BlueZ holds a bond for this node. A node in PIN mode that is not paired
       connects and then fails, so the row says so before the user presses A. Always true for
       a USB port, which has nothing to pair. */
    bool paired;
    /* The link this row is being brought up on right now (connecting, or bonding). */
    bool busy;
    /* USB only: this device is a node sitting in its UF2 bootloader rather than running
       firmware. It cannot carry a session, and the row says so instead of disappearing - the
       Devices tab is where "why will this not connect" is answered, and a board in its
       bootloader is the most answerable version of that question there is. */
    bool bootloader;
    uint8_t kind; /* enum mesh_ui_device_kind */
};

/*
 * What A and Y would do on a Devices row, asked by the press and by the action bar.
 *
 * Same reasoning as mesh_ui_snapshot_connected_device(), one level down: a bar that names a
 * keycap the press
 * then declines is the keycap-that-does-nothing `src/ui/tables/actions.c` refuses everywhere else,
 * and the only way two files stay agreed about it is to give them one function to ask. Both of
 * these were conditions written into the nav's handlers with an unconditional entry in the bar
 * beside them, which is exactly how that disagreement arises.
 *
 * NULL is false for both, so a cursor past the end needs no separate test at either call site.
 */
/* A opens a link: a row with an address, not already the one we are on, and not a node sitting
   in its bootloader - which has no session to offer however good the cable is. */
bool mesh_ui_device_connectable(const struct mesh_ui_device *device);
/* Y forgets a bond, so only a BLE row has one to forget; a USB port has nothing to bond. */
bool mesh_ui_device_forgettable(const struct mesh_ui_device *device);

#ifdef __cplusplus
}
#endif
