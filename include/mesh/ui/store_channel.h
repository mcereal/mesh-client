#pragma once

/*
 * A channel slot, twice: the cached form the roster carries and the full form the settings
 * editor needs.
 *
 * Two records rather than one because they have different lifetimes and different secrets.
 * `mesh_ui_channel` is what a list draws and what the handshake cache persists, so it carries
 * a key's *length* and not the key; `mesh_ui_channel_detail` is everything set_channel writes
 * back, keys included, and lives in the settings, which are never persisted.
 *
 * See mesh/ui/store.h for the whole.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_UI_MAX_CHANNELS 8U
#define MESH_UI_CHANNEL_NAME_MAX 12U

struct mesh_ui_channel {
    uint8_t index;
    uint8_t role; /* 0 disabled, 1 primary, 2 secondary (meshtastic_Channel_Role) */
    char name[MESH_UI_CHANNEL_NAME_MAX];
    uint8_t psk_len; /* 0 none, 1 default-key index, 16 AES-128, 32 AES-256 */
    bool uplink_enabled;
    bool downlink_enabled;
    uint32_t position_precision;
};

/* One channel slot with everything set_channel needs, keys included. Lives in the settings
   (never persisted) rather than the cached handshake. */
#define MESH_UI_PSK_MAX 32U

/*
 * Mesh beacon's two sizes, from the protobuf: `broadcast_message` is 101 bytes on the wire and
 * `broadcast_targets` holds four entries. The message is the field the edit buffer is now
 * measured to (mesh/ui/settings_text.def), and four targets is the wire's cap rather than a
 * screenful - the section lists all four, empty ones included.
 */
#define MESH_UI_BEACON_MESSAGE_MAX 101U
#define MESH_UI_BEACON_TARGETS 4U

/*
 * One broadcast destination: which radio settings a copy of the beacon goes out on.
 *
 * All three read 0 as "whatever the radio is running", which is what the wire says of the
 * region and what `optional` means for the other two. A target whose three rows are all 0 is
 * not a destination at all and is dropped from the write, which is the same compaction a
 * cleared admin key or ignore slot gets.
 */
struct mesh_ui_beacon_target {
    uint32_t preset;  /* 0 = running config, else ModemPreset n-1 */
    uint32_t region;  /* RegionCode; 0 is the wire's own UNSET */
    uint32_t channel; /* 0 = the preset's default channel, else channel index n-1 */
};

struct mesh_ui_channel_detail {
    bool present;
    uint8_t index;
    uint8_t role; /* meshtastic_Channel_Role */
    char name[MESH_UI_CHANNEL_NAME_MAX];
    uint8_t psk[MESH_UI_PSK_MAX];
    uint8_t psk_len;
    bool uplink_enabled;
    bool downlink_enabled;
    uint32_t position_precision;
    /* The other half of ChannelSettings.module_settings, which this client has been reading the
       first half of since phase 3. Muting is per channel and lives on the radio, so a busy
       public channel can be quietened without disabling it and losing the key. */
    bool is_muted;
};

#ifdef __cplusplus
}
#endif
