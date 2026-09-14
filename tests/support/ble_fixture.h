#ifndef MESH_TEST_SUPPORT_BLE_FIXTURE_H
#define MESH_TEST_SUPPORT_BLE_FIXTURE_H

/*
 * A BLE transport standing on the bluez mock: the twenty lines every link test used to open with.
 *
 * Three suites - transport_ble_link.c, transport_ble_data.c, transport_ble_pairing.c - each began
 * a case by declaring a one-device `mock_devices[]`, a write-capture quintet, a read-payload
 * script, and a `mesh_bluez_mock_config` whose first six fields were `= 0` (the designated
 * initializer's own default, so they said nothing), then enabling the mock, defaulting an app
 * config, initialising a loop and starting the transport. Seventeen cases opened that way and
 * fourteen closed with the same three-call teardown.
 *
 * What the duplication cost was not length. The three characteristic paths are
 * `<adapter>/dev_<address with colons as underscores>/service000a/char000{b,d,f}`, spelled out by
 * hand at every site: a path typed against the wrong device address gives a test that connects
 * and then reads nothing, and the failure it reports is about a missing packet. Deriving them
 * from the address is what this is for; the collapsed boilerplate comes along with it.
 *
 * The rig owns its buffers, so it is a stack local in the test and `rig.mock` stays writable:
 * anything the fixture does not set - a pending-poll count, a late write failure, a second
 * service UUID - a test sets on `rig.mock` before it starts, which is why init and start are two
 * calls rather than one.
 */

#include "mesh/core/config.h"
#include "mesh/core/event_loop.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"

#include "meshtastic/mesh.pb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESH_TEST_BLE_MAX_DEVICES 4U
/*
 * Sized for the longest script any case needs - the settings write, which drives a passkey
 * refresh, a set and a read-back past one another. The struct is a stack local of a few kilobytes
 * because of it, which is what the cases that needed 24 slots were already declaring by hand.
 */
#define MESH_TEST_BLE_MAX_READS 24U
#define MESH_TEST_BLE_READ_CAP 300U

struct mesh_test_ble_rig {
    struct mesh_transport *ble;
    struct mesh_app_config config;
    struct mesh_event_loop loop;
    struct mesh_bluez_mock_config mock;

    struct mesh_bluez_device_info devices[MESH_TEST_BLE_MAX_DEVICES];
    size_t device_count;

    /* The last ToRadio write, its path, and one length per call. */
    uint8_t write_capture[512];
    size_t write_len;
    char write_path[160];
    size_t write_call_count;
    size_t write_lengths[16];

    /* Scripted FromRadio reads. An unscripted slot stays zero-length, which the mock answers as
       an empty FIFO - the same thing it does for a read past read_payload_count. */
    uint8_t read_buffers[MESH_TEST_BLE_MAX_READS][MESH_TEST_BLE_READ_CAP];
    const uint8_t *read_payloads[MESH_TEST_BLE_MAX_READS];
    size_t read_payload_lengths[MESH_TEST_BLE_MAX_READS];
    size_t read_index;

    char toradio_path[128];
    char fromradio_path[128];
    char fromnum_path[128];

    bool started;
};

/*
 * Wires the rig to the BLE transport and the mock without enabling either: one device at
 * `address`, the captures and the read script plumbed, the characteristic paths derived. The
 * device is `paired` - a node that answers StartNotify - because all but three cases want that;
 * clear `rig->devices[0].paired` for the ones that do not.
 */
void mesh_test_ble_rig_init(struct mesh_test_ble_rig *rig, const char *address, const char *name,
                            int16_t rssi);

/*
 * Appends another advertiser. The characteristic paths keep pointing at device 0, so the extra
 * ones are what a ranking or listing test scans past rather than connects to.
 */
bool mesh_test_ble_rig_add_device(struct mesh_test_ble_rig *rig, const char *address,
                                  const char *name, int16_t rssi);

/* Enables the mock, defaults an app config, brings up a loop and starts the transport. */
int mesh_test_ble_rig_start(struct mesh_test_ble_rig *rig);

/*
 * Hands the mock the current `rig->mock` again. `mesh_bluez_client_mock_enable` copies the
 * config by value, so a test that changes a field mid-case - failing writes, then allowing them
 * again - has to say so. Resets the mock's counters, exactly as a bare re-enable did.
 */
void mesh_test_ble_rig_reload(struct mesh_test_ble_rig *rig);

/* Refreshes the device list and connects to device 0. */
int mesh_test_ble_rig_connect(struct mesh_test_ble_rig *rig);

/*
 * Encodes `message` into read slot `slot`, so the drain that reaches it hands the transport that
 * FromRadio. The slot is explicit rather than appended because the cases that script a long
 * conversation fill it out of order, leaving the gaps they want answered as empty reads.
 *
 * False when the slot is past the script or the message does not fit one - both the test asking
 * for more than the rig holds, not a radio behaving oddly.
 */
bool mesh_test_ble_rig_script(struct mesh_test_ble_rig *rig, size_t slot,
                              const meshtastic_FromRadio *message);

/* Stops the transport, shuts the loop down and disables the mock. Safe before a start. */
void mesh_test_ble_rig_close(struct mesh_test_ble_rig *rig);

#endif /* MESH_TEST_SUPPORT_BLE_FIXTURE_H */
