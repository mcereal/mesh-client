#include "mesh/proto/ble_profile.h"

const struct mesh_ble_profile mesh_ble_profile_meshtastic = {
    .name = "meshtastic",
    .service_uuid = MESH_BLE_MESHTASTIC_SERVICE_UUID,
    .write_uuid = MESH_BLE_TORADIO_UUID,
    .notify_uuid = MESH_BLE_FROMNUM_UUID,
    .read_uuid = MESH_BLE_FROMRADIO_UUID,
    .log_uuid = MESH_BLE_LOGRADIO_UUID,
    .inbound = MESH_BLE_INBOUND_PULL,
    .max_frame = MESH_BLE_MAX_PACKET_SIZE,
    .adopts_bonded_dfu = true,
};

const struct mesh_ble_profile mesh_ble_profile_meshcore = {
    .name = "meshcore",
    .service_uuid = MESH_BLE_NUS_SERVICE_UUID,
    .write_uuid = MESH_BLE_NUS_RX_UUID,
    .notify_uuid = MESH_BLE_NUS_TX_UUID,
    .inbound = MESH_BLE_INBOUND_NOTIFY,
    .max_frame = 176U,
};

const struct mesh_ble_profile *const mesh_ble_known_profiles[] = {
    &mesh_ble_profile_meshtastic,
};

const size_t mesh_ble_known_profile_count =
    sizeof mesh_ble_known_profiles / sizeof mesh_ble_known_profiles[0];

bool mesh_ble_profile_usable(const struct mesh_ble_profile *profile) {
    if (profile == NULL || profile->service_uuid == NULL || profile->write_uuid == NULL ||
        profile->notify_uuid == NULL || profile->max_frame == 0U ||
        profile->max_frame > MESH_BLE_MAX_PACKET_SIZE) {
        return false;
    }
    return profile->inbound != MESH_BLE_INBOUND_PULL || profile->read_uuid != NULL;
}
