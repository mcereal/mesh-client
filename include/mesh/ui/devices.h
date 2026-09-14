#pragma once

/*
 * ---- the Devices tab's rows -------------------------------------------------------------------
 *
 * What the list is made of, as a table read by everything that has an opinion about it -
 * src/ui/nav.c for the row count and what a press on a row does, src/ui/actions.c for the verb
 * that press gets named with, and the backend for what the row draws. That is src/ui/nodes.c's
 * shape one tab over, and it is here for nodes.c's reason: three files walking the same list is
 * how the row under the cursor and the verb under the keycaps come to disagree.
 *
 * Every row but one is a device discovery found. The exception is the last one, and it exists
 * because a network cannot be scanned: the TCP link ships, works and reaches a node over WiFi,
 * and until this row the only way to point it anywhere was to edit `launch.sh` on a memory card.
 * A transport reachable only from a text editor is not a transport a handheld has.
 *
 * The network row is present exactly when the device list holds no network row of its own.
 * mesh_app_publish_ui_state() synthesises one while a network link is up - that is the only row
 * a TCP link will ever have, since nothing enumerates a host - so the two would otherwise be two
 * rows about one address. Being last in both cases, the row does not move when the link comes
 * up: it stops being the button that configures a host and becomes the device that is one.
 */

#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_ui_devices_row_type {
    /* A radio discovery found: a Bluetooth bond, a USB port, or the network link that is up. */
    MESH_UI_DEVICES_ROW_DEVICE = 0,
    /*
     * The last row: the network address, and the way to type one.
     *
     * A row rather than a keycap on the app bar, for the reason the Waypoints tab's "New
     * waypoint here" is one - a list whose only way to add to it is a press nothing on the
     * screen mentions is a list people believe is read-only. It says why it cannot connect
     * ("Set an address") rather than disappearing, which is the same rule again.
     */
    MESH_UI_DEVICES_ROW_NETWORK,
};

/* One row of the list, resolved against the snapshot it was built from. */
struct mesh_ui_devices_row {
    uint8_t type; /* enum mesh_ui_devices_row_type */
    /* DEVICE only: borrowed from the store this was asked of, and valid for as long as it is. */
    const struct mesh_ui_device *device;
    /* NETWORK only: the configured address, or "" when nobody has written one down. Never
       NULL, so a caller may print it without a guard. */
    const char *host;
};

/*
 * How many rows the list has: every device, then the network row when the devices do not
 * already include one. Never 0 - which is what makes the Devices tab usable on a handheld with
 * no Bluetooth adapter and nothing plugged in.
 *
 * The published list rather than the store holding it, for src/ui/nodes.c's reason: the nav asks
 * this of a `struct mesh_ui_store` and the action bar and the renderer ask it of a
 * `struct mesh_ui_snapshot`, and taking one of the two would make the other copy a hundred
 * kilobytes onto the stack, every frame, to read two fields.
 */
uint32_t mesh_ui_devices_row_count(const struct mesh_ui_device *devices, size_t count);

/* Describes row `index`. False past the end. `network_host` may be NULL for none. */
bool mesh_ui_devices_row(const struct mesh_ui_device *devices, size_t count,
                         const char *network_host, uint32_t index,
                         struct mesh_ui_devices_row *out);

#ifdef __cplusplus
}
#endif
