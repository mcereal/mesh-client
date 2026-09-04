#pragma once

/*
 * The connected radio's configuration, as the radio reports it, plus the AdminMessage
 * plumbing needed to ask for it again and (later) to change it. Transport-agnostic: the BLE
 * transport feeds FromRadio fragments and ADMIN_APP packets in and pulls encoded ToRadio
 * requests out; serial/TCP would do the same. docs/settings-roadmap.md explains the plan.
 *
 * Every admin reply carries a session passkey that a later set_* must echo back (firmware
 * 2.5+ rejects a write without one, and keys live for five minutes). Phase 1 only reads, but
 * the passkey is captured and reported so the write path can be proven on hardware first.
 */

#include "meshtastic/admin.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/module_config.pb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a request asks for. GET_CONFIG and GET_MODULE_CONFIG carry the section type. */
enum mesh_admin_request_kind {
    MESH_ADMIN_GET_OWNER = 0,
    MESH_ADMIN_GET_METADATA,
    MESH_ADMIN_GET_CONFIG,        /* type = meshtastic_AdminMessage_ConfigType */
    MESH_ADMIN_GET_MODULE_CONFIG, /* type = meshtastic_AdminMessage_ModuleConfigType */
};

struct mesh_admin_request {
    enum mesh_admin_request_kind kind;
    uint32_t type;
    uint32_t my_node;   /* MeshPacket.to: admin goes to ourselves */
    uint32_t packet_id; /* MeshPacket.id; replies quote it in Data.request_id */
};

/* Sections we fetch on a full refresh, in the order they are asked for. */
#define MESH_RADIO_SETTINGS_FETCH_MAX 16U
/* A reply that has not arrived after this long is given up on and the queue moves on. */
#define MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS 5000U

struct mesh_radio_settings {
    bool has_device;
    meshtastic_Config_DeviceConfig device;
    bool has_position;
    meshtastic_Config_PositionConfig position;
    bool has_power;
    meshtastic_Config_PowerConfig power;
    bool has_network;
    meshtastic_Config_NetworkConfig network;
    bool has_display;
    meshtastic_Config_DisplayConfig display;
    bool has_lora;
    meshtastic_Config_LoRaConfig lora;
    bool has_bluetooth;
    meshtastic_Config_BluetoothConfig bluetooth;
    bool has_security;
    meshtastic_Config_SecurityConfig security;
    bool has_mqtt;
    meshtastic_ModuleConfig_MQTTConfig mqtt;
    bool has_store_forward;
    meshtastic_ModuleConfig_StoreForwardConfig store_forward;
    bool has_telemetry;
    meshtastic_ModuleConfig_TelemetryConfig telemetry;
    bool has_owner;
    meshtastic_User owner;
    bool has_metadata;
    meshtastic_DeviceMetadata metadata;

    /* Admin session. */
    bool has_session_passkey;
    uint8_t session_passkey[8];
    size_t session_passkey_len;
    uint32_t admin_replies; /* ADMIN_APP replies decoded since reset */

    /* One-at-a-time fetch queue. */
    struct mesh_admin_request queue[MESH_RADIO_SETTINGS_FETCH_MAX];
    size_t queue_head;
    size_t queue_len;
    uint32_t pending_request_id; /* 0 = nothing in flight */
    uint64_t pending_sent_at_ms;
    unsigned timeouts;
};

void mesh_radio_settings_reset(struct mesh_radio_settings *settings);

/* True once any section, the owner or the metadata has arrived. */
bool mesh_radio_settings_loaded(const struct mesh_radio_settings *settings);

/* Fold in fragments the radio streams during the want_config handshake. */
void mesh_radio_settings_apply_config(struct mesh_radio_settings *settings,
                                      const meshtastic_Config *config);
void mesh_radio_settings_apply_module_config(struct mesh_radio_settings *settings,
                                             const meshtastic_ModuleConfig *config);
void mesh_radio_settings_apply_metadata(struct mesh_radio_settings *settings,
                                        const meshtastic_DeviceMetadata *metadata);
void mesh_radio_settings_apply_owner(struct mesh_radio_settings *settings,
                                     const meshtastic_User *owner);

/* Folds an ADMIN_APP packet in: captures the session passkey, stores whatever get_*_response
   it carries, and releases the fetch queue when it answers the pending request. Returns 1
   when the packet was an admin reply (the caller should not treat it as a message), 0 when
   it was something else, a negative errno on bad input. */
int mesh_radio_settings_ingest(struct mesh_radio_settings *settings,
                               const meshtastic_MeshPacket *packet);

/* Encodes one request as a ToRadio protobuf ready for a single BLE write: a MeshPacket to
   ourselves on ADMIN_APP with want_response set, carrying the passkey we hold (harmless on a
   get; required on a set). Returns 0 and sets *written on success. */
int mesh_radio_settings_encode_request(const struct mesh_radio_settings *settings,
                                       const struct mesh_admin_request *request, uint8_t *out,
                                       size_t out_len, size_t *written);

/* Fetch queue. queue_probe() asks for the owner and the metadata (enough to prove the admin
   round trip); queue_all() asks for everything the Settings tab shows. Duplicates of a kind
   already queued are skipped. Returns the number of requests added. */
size_t mesh_radio_settings_queue_probe(struct mesh_radio_settings *settings);
size_t mesh_radio_settings_queue_all(struct mesh_radio_settings *settings);

/* Hands out the next request to send when nothing is in flight (or the in-flight one has
   timed out). The caller fills in packet_id/my_node, encodes and sends it, then calls
   mark_sent(). Returns false when there is nothing to send right now. */
bool mesh_radio_settings_next_request(struct mesh_radio_settings *settings, uint64_t now_ms,
                                      struct mesh_admin_request *out);
void mesh_radio_settings_mark_sent(struct mesh_radio_settings *settings, uint32_t packet_id,
                                   uint64_t now_ms);
/* True while a reply is awaited. */
bool mesh_radio_settings_busy(const struct mesh_radio_settings *settings);

/* Human names for the enums the Settings tab shows; "?" / a numeric fallback when unknown. */
const char *mesh_radio_role_name(uint32_t role);
const char *mesh_radio_region_name(uint32_t region);
const char *mesh_radio_modem_preset_name(uint32_t preset);
const char *mesh_radio_hw_model_name(uint32_t model, char *fallback, size_t fallback_len);

#ifdef __cplusplus
}
#endif
