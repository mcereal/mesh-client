#include "mesh/transport/ble_hci.h"

int mesh_ble_ota_request_interval(struct inkwell_ble_central *central, const char *address) {
    /* The loader asks for 7.5 ms, no latency and a four-second supervision timeout. */
    static const struct inkwell_ble_connection_parameters k_ota = {
        .min_interval = 6U,
        .max_interval = 6U,
        .latency = 0U,
        .supervision_timeout = 400U,
    };
    return inkwell_ble_request_connection_interval(central, address, &k_ota);
}
