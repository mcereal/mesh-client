#include "mesh/core/radio_settings.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"

#include "meshtastic/portnums.pb.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void mesh_radio_settings_reset(struct mesh_radio_settings *settings) {
    if (settings == NULL) {
        return;
    }
    memset(settings, 0, sizeof *settings);
}

void mesh_radio_settings_reset_session(struct mesh_radio_settings *settings) {
    if (settings == NULL) {
        return;
    }
    /*
     * The preset map goes with the link, which is the one place it differs from every other
     * thing the radio told us about itself.
     *
     * Those are all kept across a reconnect because the handshake that follows *overwrites*
     * each section as its fragment lands, so keeping them costs nothing and saves the client
     * every screen it could have drawn during the replay. The preset map is the one whose
     * **absence** is the message: a firmware that predates it sends nothing, so there is
     * nothing to overwrite with, and a map held from a previous connection would stand for
     * ever. A radio downgraded behind our back is a reflash, which we never see on the wire -
     * so the map cannot outlive the handshake that carried it.
     *
     * It costs nothing to drop: the firmware sends it early (right after the metadata, before
     * the first channel), and until it arrives every region is unconstrained, which is what a
     * client that knows nothing should do.
     */
    settings->has_region_presets = false;
    memset(&settings->region_presets, 0, sizeof settings->region_presets);

    settings->has_session_passkey = false;
    memset(settings->session_passkey, 0, sizeof settings->session_passkey);
    settings->session_passkey_len = 0U;
    settings->admin_replies = 0U;

    /*
     * The admin target goes with the link too, and it is on that side of the line for the
     * passkey's reason rather than the preset map's: remote administration is a conversation
     * held *through* a radio, so it cannot outlive the radio it was being held through. A
     * reconnect comes back pointed at its own radio, which is also the only thing a client that
     * has just found a link can safely assume it is looking at.
     */
    settings->admin_dest = 0U;
    memset(settings->admin_dest_key, 0, sizeof settings->admin_dest_key);
    settings->admin_dest_key_len = 0U;

    memset(settings->queue, 0, sizeof settings->queue);
    settings->queue_head = 0U;
    settings->queue_len = 0U;
    settings->pending_request_id = 0U;
    settings->pending_sent_at_ms = 0U;
    settings->pending_is_write = false;
    settings->pending_dest = 0U;
    settings->timeouts = 0U;
    settings->remote_silence = 0U;
    settings->local_silence = 0U;
    settings->link_silent = false;

    /* Counted so the app can announce each outcome once, which makes them facts about the link
       that reported them rather than about the radio. */
    settings->writes_sent = 0U;
    settings->writes_acked = 0U;
    settings->writes_failed = 0U;
    settings->last_write_error = 0;
}

bool mesh_radio_settings_loaded(const struct mesh_radio_settings *settings) {
    if (settings == NULL) {
        return false;
    }
    /* The contract is "anything has arrived". The Config sections are still listed by hand -
       there are eight of them and they do not grow - but the modules come from the table, so a
       new one counts here the moment its row exists. Forgetting this list is what made a radio
       read as absent while we held its config.

       `has_region_presets` is deliberately not on it, and the distinction is what the predicate
       is about: every flag here is a section the tab *draws*, and the preset map is a table that
       constrains one row of one of them. A radio that had sent nothing else would otherwise read
       as loaded with every screen in the tab still empty. */
    if (settings->has_device || settings->has_position || settings->has_power ||
        settings->has_network || settings->has_display || settings->has_lora ||
        settings->has_bluetooth || settings->has_security || settings->has_owner ||
        settings->has_metadata || settings->has_ui_config || settings->has_connection_status ||
        settings->has_canned_messages || settings->has_ringtone) {
        return true;
    }
    for (size_t i = 0; i < mesh_radio_module_count(); ++i) {
        if (mesh_radio_module_held(settings, mesh_radio_module_at(i))) {
            return true;
        }
    }
    for (size_t i = 0; i < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++i) {
        if (settings->has_channel[i]) {
            return true;
        }
    }
    return false;
}

/* ---- the admin target --------------------------------------------------------------------- */

uint32_t mesh_radio_settings_admin_dest(const struct mesh_radio_settings *settings) {
    return settings == NULL ? 0U : settings->admin_dest;
}

const meshtastic_DeviceMetadata *
mesh_radio_settings_link_metadata(const struct mesh_radio_settings *settings) {
    if (settings == NULL || !settings->has_link_metadata) {
        return NULL;
    }
    return &settings->link_metadata;
}

int mesh_radio_settings_set_admin_dest(struct mesh_radio_settings *settings, uint32_t node_id,
                                       const uint8_t *public_key, size_t key_len) {
    if (settings == NULL) {
        return -EINVAL;
    }
    if (node_id != 0U && (public_key == NULL || key_len != MESH_ADMIN_PUBLIC_KEY_LEN)) {
        return -EINVAL;
    }

    /*
     * Everything the old target said, forgotten in one go.
     *
     * A blunter instrument than walking the has_* flags, and deliberately so: this struct is
     * "one radio's configuration", the eight Config sections and seventeen modules are listed
     * by hand in three other places already, and a section that a future phase added and that
     * this function forgot would be that radio's settings shown under somebody else's name.
     * Clearing the whole thing and putting back what is not a section cannot make that mistake.
     */
    const uint32_t writes_sent = settings->writes_sent;
    const uint32_t writes_acked = settings->writes_acked;
    const uint32_t writes_failed = settings->writes_failed;
    const int32_t last_write_error = settings->last_write_error;
    /*
     * And the preset map, which is the one thing here that is not a fact about a *radio*: it
     * describes the firmware's own table of which modem presets each region will take, it
     * arrives unasked during the handshake, and there is no admin verb that can ask a remote
     * node for its copy. So the connected radio's is the only answer this client will ever
     * have, and dropping it would leave the LoRa section unconstrained for as long as the link
     * lasted - including after a return to our own radio, whose map would not come back until
     * the next reconnect.
     */
    const bool has_region_presets = settings->has_region_presets;
    const meshtastic_LoRaRegionPresetMap region_presets = settings->region_presets;
    /*
     * And what the radio on the end of the link said about itself, which is the other thing
     * here that is not a section. It is what decides which firmware image may be written down
     * this cable, and that question does not change because the Settings tab is now describing
     * somebody else's radio - nor could it be asked again, since the handshake that answers it
     * happens once per connection.
     */
    const bool has_link_metadata = settings->has_link_metadata;
    const meshtastic_DeviceMetadata link_metadata = settings->link_metadata;

    memset(settings, 0, sizeof *settings);

    settings->writes_sent = writes_sent;
    settings->writes_acked = writes_acked;
    settings->writes_failed = writes_failed;
    settings->last_write_error = last_write_error;
    settings->has_region_presets = has_region_presets;
    settings->region_presets = region_presets;
    settings->has_link_metadata = has_link_metadata;
    settings->link_metadata = link_metadata;

    settings->admin_dest = node_id;
    if (node_id != 0U) {
        memcpy(settings->admin_dest_key, public_key, MESH_ADMIN_PUBLIC_KEY_LEN);
        settings->admin_dest_key_len = MESH_ADMIN_PUBLIC_KEY_LEN;
    }
    if (node_id == 0U) {
        inkwell_log_info("admin", "Administering the connected radio again");
    } else {
        inkwell_log_info("admin", "Administering node 0x%08x over the mesh", node_id);
    }
    return 0;
}

/* ---- handshake fragments ------------------------------------------------------------------ */

void mesh_radio_settings_apply_config(struct mesh_radio_settings *settings,
                                      const meshtastic_Config *config) {
    if (settings == NULL || config == NULL) {
        return;
    }
    switch (config->which_payload_variant) {
    case meshtastic_Config_device_tag:
        settings->has_device = true;
        settings->device = config->payload_variant.device;
        break;
    case meshtastic_Config_position_tag:
        settings->has_position = true;
        settings->position = config->payload_variant.position;
        break;
    case meshtastic_Config_power_tag:
        settings->has_power = true;
        settings->power = config->payload_variant.power;
        break;
    case meshtastic_Config_network_tag:
        settings->has_network = true;
        settings->network = config->payload_variant.network;
        break;
    case meshtastic_Config_display_tag:
        settings->has_display = true;
        settings->display = config->payload_variant.display;
        break;
    case meshtastic_Config_lora_tag:
        settings->has_lora = true;
        settings->lora = config->payload_variant.lora;
        break;
    case meshtastic_Config_bluetooth_tag:
        settings->has_bluetooth = true;
        settings->bluetooth = config->payload_variant.bluetooth;
        break;
    case meshtastic_Config_security_tag:
        settings->has_security = true;
        settings->security = config->payload_variant.security;
        break;
    default:
        /* sessionkey and device_ui: nothing the Settings tab shows. */
        break;
    }
}

/*
 * One row per ModuleConfig section this client keeps. The admin type and the union tag sit
 * together because they are the pair that used to be typed apart and had to agree; `size` is
 * taken from the member itself so a row cannot claim a length its storage does not have.
 *
 * Adding a module is this row plus its storage in the struct - the apply, the loaded predicate
 * and the refresh queue all read it from here.
 */
#define MODULE_BINDING(admin, tag, flag, member)                                                   \
    {                                                                                              \
        (uint32_t)(admin), (uint32_t)(tag), offsetof(struct mesh_radio_settings, flag),            \
            offsetof(struct mesh_radio_settings, member),                                          \
            sizeof(((struct mesh_radio_settings *)0)->member)                                      \
    }

static const struct mesh_module_binding k_modules[] = {
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_MQTT_CONFIG,
                   meshtastic_ModuleConfig_mqtt_tag, has_mqtt, mqtt),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_STOREFORWARD_CONFIG,
                   meshtastic_ModuleConfig_store_forward_tag, has_store_forward, store_forward),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_TELEMETRY_CONFIG,
                   meshtastic_ModuleConfig_telemetry_tag, has_telemetry, telemetry),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_NEIGHBORINFO_CONFIG,
                   meshtastic_ModuleConfig_neighbor_info_tag, has_neighbor_info, neighbor_info),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_RANGETEST_CONFIG,
                   meshtastic_ModuleConfig_range_test_tag, has_range_test, range_test),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_PAXCOUNTER_CONFIG,
                   meshtastic_ModuleConfig_paxcounter_tag, has_paxcounter, paxcounter),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_TAK_CONFIG,
                   meshtastic_ModuleConfig_tak_tag, has_tak, tak),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_AMBIENTLIGHTING_CONFIG,
                   meshtastic_ModuleConfig_ambient_lighting_tag, has_ambient_lighting,
                   ambient_lighting),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_STATUSMESSAGE_CONFIG,
                   meshtastic_ModuleConfig_statusmessage_tag, has_status_message, status_message),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_DETECTIONSENSOR_CONFIG,
                   meshtastic_ModuleConfig_detection_sensor_tag, has_detection_sensor,
                   detection_sensor),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_EXTNOTIF_CONFIG,
                   meshtastic_ModuleConfig_external_notification_tag, has_external_notification,
                   external_notification),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_TRAFFICMANAGEMENT_CONFIG,
                   meshtastic_ModuleConfig_traffic_management_tag, has_traffic_management,
                   traffic_management),
    MODULE_BINDING(meshtastic_AdminMessage_ModuleConfigType_MESHBEACON_CONFIG,
                   meshtastic_ModuleConfig_mesh_beacon_tag, has_mesh_beacon, mesh_beacon),
};

size_t mesh_radio_module_count(void) { return sizeof k_modules / sizeof k_modules[0]; }

const struct mesh_module_binding *mesh_radio_module_at(size_t index) {
    return index < mesh_radio_module_count() ? &k_modules[index] : NULL;
}

const struct mesh_module_binding *mesh_radio_module_for_type(uint32_t admin_type) {
    for (size_t i = 0; i < mesh_radio_module_count(); ++i) {
        if (k_modules[i].admin_type == admin_type) {
            return &k_modules[i];
        }
    }
    return NULL;
}

/* The has_* flag and the kept section, reached through the row's offsets. Separate helpers
   because everything below wants one or the other and none of them should be doing pointer
   arithmetic of its own. */
static bool *module_flag(struct mesh_radio_settings *settings,
                         const struct mesh_module_binding *binding) {
    return (bool *)((char *)settings + binding->has_offset);
}

static void *module_store(struct mesh_radio_settings *settings,
                          const struct mesh_module_binding *binding) {
    return (char *)settings + binding->store_offset;
}

bool mesh_radio_module_held(const struct mesh_radio_settings *settings,
                            const struct mesh_module_binding *binding) {
    if (settings == NULL || binding == NULL) {
        return false;
    }
    return *(const bool *)((const char *)settings + binding->has_offset);
}

bool mesh_radio_module_load(const struct mesh_radio_settings *settings,
                            const struct mesh_module_binding *binding,
                            meshtastic_ModuleConfig *out) {
    if (out == NULL || !mesh_radio_module_held(settings, binding)) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->which_payload_variant = (pb_size_t)binding->variant_tag;
    memcpy(&out->payload_variant, (const char *)settings + binding->store_offset, binding->size);
    return true;
}

void mesh_radio_settings_apply_module_config(struct mesh_radio_settings *settings,
                                             const meshtastic_ModuleConfig *config) {
    if (settings == NULL || config == NULL) {
        return;
    }
    /* Every payload_variant member shares the union's address, so the tag alone says which
       section this is and the row says where it goes and how much of it there is. */
    for (size_t i = 0; i < mesh_radio_module_count(); ++i) {
        const struct mesh_module_binding *binding = &k_modules[i];
        if (binding->variant_tag != (uint32_t)config->which_payload_variant) {
            continue;
        }
        *module_flag(settings, binding) = true;
        memcpy(module_store(settings, binding), &config->payload_variant, binding->size);
        return;
    }
}

void mesh_radio_settings_apply_metadata(struct mesh_radio_settings *settings,
                                        const meshtastic_DeviceMetadata *metadata) {
    if (settings == NULL || metadata == NULL) {
        return;
    }
    settings->has_metadata = true;
    settings->metadata = *metadata;
    /*
     * And the link's own copy, while there is nothing else being administered.
     *
     * With no remote target every metadata that reaches here is the connected radio's - the
     * handshake's FromRadio and an admin reply are the only two ways in, and the second is
     * already filtered to the target by mesh_radio_settings_reply_is_targets(). So this one
     * branch is the whole of keeping the two apart, and what it buys is a firmware install that
     * still knows which board it is writing to.
     */
    if (settings->admin_dest == 0U) {
        settings->has_link_metadata = true;
        settings->link_metadata = *metadata;
    }
}

void mesh_radio_settings_apply_owner(struct mesh_radio_settings *settings,
                                     const meshtastic_User *owner) {
    if (settings == NULL || owner == NULL) {
        return;
    }
    settings->has_owner = true;
    settings->owner = *owner;
}

void mesh_radio_settings_apply_channel(struct mesh_radio_settings *settings,
                                       const meshtastic_Channel *channel) {
    if (settings == NULL || channel == NULL || channel->index < 0 ||
        (size_t)channel->index >= MESH_RADIO_SETTINGS_MAX_CHANNELS) {
        return;
    }
    settings->has_channel[channel->index] = true;
    settings->channels[channel->index] = *channel;
}

/*
 * The radio's own screen configuration. Arrives twice over: unasked as FromRadio.deviceuiConfig
 * while the handshake streams, and again as get_ui_config_response on a refresh. Kept whole -
 * see the note on `ui_config` in the header for why a partial copy would lose a calibration.
 */
void mesh_radio_settings_apply_ui_config(struct mesh_radio_settings *settings,
                                         const meshtastic_DeviceUIConfig *config) {
    if (settings == NULL || config == NULL) {
        return;
    }
    settings->has_ui_config = true;
    settings->ui_config = *config;
}

void mesh_radio_settings_apply_region_presets(struct mesh_radio_settings *settings,
                                              const meshtastic_LoRaRegionPresetMap *map) {
    if (settings == NULL || map == NULL) {
        return;
    }
    settings->has_region_presets = true;
    settings->region_presets = *map;
}

/* ---- admin replies ------------------------------------------------------------------------ */

bool mesh_admin_request_is_write(enum mesh_admin_request_kind kind) {
    /* MESH_ADMIN_SET_TIME is a set_* on the wire but not here: see the header. The two
       fixed-position kinds *are* writes: they are a save the user pressed for, they are acked
       like any other, and the get_config POSITION behind them makes the row show what the
       radio actually kept. */
    return kind == MESH_ADMIN_SET_OWNER || kind == MESH_ADMIN_SET_CONFIG ||
           kind == MESH_ADMIN_SET_MODULE_CONFIG || kind == MESH_ADMIN_SET_CHANNEL ||
           kind == MESH_ADMIN_SET_FIXED_POSITION || kind == MESH_ADMIN_REMOVE_FIXED_POSITION ||
           kind == MESH_ADMIN_SET_UI_CONFIG || kind == MESH_ADMIN_SET_CANNED_MESSAGES;
}

bool mesh_admin_request_is_action(enum mesh_admin_request_kind kind) {
    /* The backup trio is here rather than among the writes for the reason the resets are:
       nothing is read back, and what they move is the radio's whole stored configuration
       rather than a section this client has rows for. A restore does change what the radio
       holds, so the caller follows it with a refresh - that is a read, not a read-back. */
    /* Ham mode is here rather than among the writes for the backup trio's reason, sharpened:
       one verb moves the owner's names, the primary channel's key and LoRa's own frequency,
       so there is no single section a read-back could ask for. The caller refreshes. */
    return kind == MESH_ADMIN_REBOOT || kind == MESH_ADMIN_SHUTDOWN ||
           kind == MESH_ADMIN_RESET_NODEDB || kind == MESH_ADMIN_FACTORY_RESET_CONFIG ||
           kind == MESH_ADMIN_FACTORY_RESET_DEVICE || kind == MESH_ADMIN_ENTER_DFU_MODE ||
           kind == MESH_ADMIN_BACKUP_PREFERENCES || kind == MESH_ADMIN_RESTORE_PREFERENCES ||
           kind == MESH_ADMIN_REMOVE_BACKUP_PREFERENCES || kind == MESH_ADMIN_OTA_REQUEST ||
           kind == MESH_ADMIN_SET_HAM_MODE;
}

static void mesh_radio_settings_record_write_result(struct mesh_radio_settings *settings,
                                                    int32_t error) {
    if (error == 0) {
        settings->writes_acked += 1U;
    } else {
        settings->writes_failed += 1U;
        settings->last_write_error = error;
    }
}

/* Releases the queue when `request_id` answers the request in flight. `error` is the Routing
   error the reply carried (0 for an AdminMessage reply or a clean ack). */
static bool mesh_radio_settings_finish_pending(struct mesh_radio_settings *settings,
                                               uint32_t request_id, int32_t error) {
    if (settings->pending_request_id == 0U || settings->pending_request_id != request_id) {
        return false;
    }
    if (settings->pending_is_write) {
        mesh_radio_settings_record_write_result(settings, error);
    }
    settings->pending_request_id = 0U;
    settings->pending_sent_at_ms = 0U;
    settings->pending_is_write = false;
    settings->pending_dest = 0U;
    /* Something answered, so the mesh is carrying our admin traffic: the give-up count starts
       again. A Routing rejection counts - it is still the far end talking to us. */
    settings->remote_silence = 0U;
    settings->local_silence = 0U;
    return true;
}

/*
 * Whether a get_*_response may be folded into the sections this struct keeps.
 *
 * The question only exists because the queue can be pointed at somebody else's radio while
 * still carrying requests for our own - the clock push and the NodeDB verbs never follow the
 * target - and a get_owner sent for one of those would otherwise overwrite the remote node's
 * owner with ours, in the very rows the Settings tab is showing under that node's name.
 *
 * Answered from the request in flight rather than from `packet->from`, because the reply is
 * already correlated by id and that correlation is the one this queue is built on: a client
 * that also had to recognise its own node number would need to be told what it is.
 *
 * With no remote target set nothing changes - every reply is this radio's, including an
 * unsolicited one that answers no request we are holding.
 */
static bool mesh_radio_settings_reply_is_targets(const struct mesh_radio_settings *settings,
                                                 uint32_t request_id) {
    if (settings->admin_dest == 0U) {
        return true;
    }
    return request_id != 0U && settings->pending_request_id == request_id &&
           settings->pending_dest == settings->admin_dest;
}

/* The ack (or rejection) of a set_*: a Routing packet quoting our packet id. */
static int mesh_radio_settings_ingest_routing(struct mesh_radio_settings *settings,
                                              const meshtastic_MeshPacket *packet) {
    const uint32_t request_id = packet->decoded.request_id;
    if (request_id == 0U || settings->pending_request_id != request_id) {
        return 0; /* answers a text message or something else; not ours */
    }
    meshtastic_Routing routing = meshtastic_Routing_init_default;
    pb_istream_t stream =
        pb_istream_from_buffer(packet->decoded.payload.bytes, packet->decoded.payload.size);
    int32_t error = 0;
    if (!pb_decode(&stream, meshtastic_Routing_fields, &routing)) {
        inkwell_log_warn("admin", "Undecodable Routing reply to admin request %u: %s", request_id,
                         PB_GET_ERROR(&stream));
    } else if (routing.which_variant == meshtastic_Routing_error_reason_tag) {
        error = (int32_t)routing.error_reason;
    }
    if (error == 0) {
        inkwell_log_info("admin", "Admin request %u acknowledged", request_id);
    } else {
        inkwell_log_warn("admin", "Admin request %u rejected: routing error %d", request_id,
                         (int)error);
    }
    mesh_radio_settings_finish_pending(settings, request_id, error);
    return 1;
}

int mesh_radio_settings_ingest(struct mesh_radio_settings *settings,
                               const meshtastic_MeshPacket *packet) {
    if (settings == NULL || packet == NULL) {
        return -EINVAL;
    }
    if (packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return 0;
    }
    if (packet->decoded.portnum == meshtastic_PortNum_ROUTING_APP) {
        return mesh_radio_settings_ingest_routing(settings, packet);
    }
    if (packet->decoded.portnum != meshtastic_PortNum_ADMIN_APP) {
        return 0;
    }

    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    pb_istream_t stream =
        pb_istream_from_buffer(packet->decoded.payload.bytes, packet->decoded.payload.size);
    if (!pb_decode(&stream, meshtastic_AdminMessage_fields, &admin)) {
        inkwell_log_warn("admin", "Undecodable AdminMessage (request_id=%u): %s",
                         packet->decoded.request_id, PB_GET_ERROR(&stream));
        mesh_radio_settings_finish_pending(settings, packet->decoded.request_id, 0);
        return 1;
    }

    settings->admin_replies += 1U;
    const bool is_targets =
        mesh_radio_settings_reply_is_targets(settings, packet->decoded.request_id);
    if (admin.session_passkey.size > 0U) {
        size_t len = admin.session_passkey.size;
        if (len > sizeof settings->session_passkey) {
            len = sizeof settings->session_passkey;
        }
        memcpy(settings->session_passkey, admin.session_passkey.bytes, len);
        settings->session_passkey_len = len;
        settings->has_session_passkey = true;
    }

    /*
     * A reply that belongs to a request we sent somewhere other than the node this tab is
     * describing carries a passkey worth keeping - that is the whole of what it was asked for -
     * and nothing else. Folding its sections in would put one radio's configuration under
     * another's name, in the rows the tab is drawing under that name right now.
     */
    if (!is_targets) {
        inkwell_log_info("admin", "Admin reply for another node (request_id=%u); passkey only",
                         packet->decoded.request_id);
        mesh_radio_settings_finish_pending(settings, packet->decoded.request_id, 0);
        return 1;
    }

    const char *what = "other";
    switch (admin.which_payload_variant) {
    case meshtastic_AdminMessage_get_owner_response_tag:
        mesh_radio_settings_apply_owner(settings, &admin.get_owner_response);
        what = "owner";
        break;
    case meshtastic_AdminMessage_get_config_response_tag:
        mesh_radio_settings_apply_config(settings, &admin.get_config_response);
        what = "config";
        break;
    case meshtastic_AdminMessage_get_module_config_response_tag:
        mesh_radio_settings_apply_module_config(settings, &admin.get_module_config_response);
        what = "module config";
        break;
    case meshtastic_AdminMessage_get_device_metadata_response_tag:
        mesh_radio_settings_apply_metadata(settings, &admin.get_device_metadata_response);
        what = "metadata";
        break;
    case meshtastic_AdminMessage_get_channel_response_tag:
        mesh_radio_settings_apply_channel(settings, &admin.get_channel_response);
        what = "channel";
        break;
    case meshtastic_AdminMessage_get_device_connection_status_response_tag:
        settings->has_connection_status = true;
        settings->connection_status = admin.get_device_connection_status_response;
        what = "connection status";
        break;
    case meshtastic_AdminMessage_get_ui_config_response_tag:
        mesh_radio_settings_apply_ui_config(settings, &admin.get_ui_config_response);
        what = "ui config";
        break;
    case meshtastic_AdminMessage_get_canned_message_module_messages_response_tag:
        settings->has_canned_messages = true;
        inkwell_str_copy(settings->canned_messages, sizeof settings->canned_messages,
                         admin.get_canned_message_module_messages_response);
        what = "canned messages";
        break;
    case meshtastic_AdminMessage_get_ringtone_response_tag:
        settings->has_ringtone = true;
        inkwell_str_copy(settings->ringtone, sizeof settings->ringtone,
                         admin.get_ringtone_response);
        what = "ringtone";
        break;
    default:
        break;
    }

    inkwell_log_info("admin", "Admin reply: %s (request_id=%u, variant=%u, session passkey %s)",
                     what, packet->decoded.request_id, (unsigned)admin.which_payload_variant,
                     settings->has_session_passkey ? "held" : "absent");
    mesh_radio_settings_finish_pending(settings, packet->decoded.request_id, 0);
    return 1;
}

/* ---- requests ----------------------------------------------------------------------------- */

int mesh_radio_settings_encode_request(const struct mesh_radio_settings *settings,
                                       const struct mesh_admin_request *request, uint8_t *out,
                                       size_t out_len, size_t *written) {
    if (settings == NULL || request == NULL || out == NULL || written == NULL) {
        return -EINVAL;
    }
    if (request->packet_id == 0U) {
        return -EINVAL;
    }

    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    switch (request->kind) {
    case MESH_ADMIN_GET_OWNER:
        admin.which_payload_variant = meshtastic_AdminMessage_get_owner_request_tag;
        admin.get_owner_request = true;
        break;
    case MESH_ADMIN_GET_METADATA:
        admin.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_request_tag;
        admin.get_device_metadata_request = true;
        break;
    case MESH_ADMIN_GET_CONFIG:
        admin.which_payload_variant = meshtastic_AdminMessage_get_config_request_tag;
        admin.get_config_request = (meshtastic_AdminMessage_ConfigType)request->type;
        break;
    case MESH_ADMIN_GET_MODULE_CONFIG:
        admin.which_payload_variant = meshtastic_AdminMessage_get_module_config_request_tag;
        admin.get_module_config_request = (meshtastic_AdminMessage_ModuleConfigType)request->type;
        break;
    case MESH_ADMIN_SET_OWNER:
        admin.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
        admin.set_owner = request->payload.owner;
        break;
    case MESH_ADMIN_SET_CONFIG:
        if (request->payload.config.which_payload_variant == 0U) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
        admin.set_config = request->payload.config;
        break;
    case MESH_ADMIN_SET_MODULE_CONFIG:
        if (request->payload.module_config.which_payload_variant == 0U) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_set_module_config_tag;
        admin.set_module_config = request->payload.module_config;
        break;
    case MESH_ADMIN_GET_CHANNEL:
        if (request->type >= MESH_RADIO_SETTINGS_MAX_CHANNELS) {
            return -EINVAL;
        }
        /* One-based on the wire so a request for slot 0 is never an absent field. */
        admin.which_payload_variant = meshtastic_AdminMessage_get_channel_request_tag;
        admin.get_channel_request = request->type + 1U;
        break;
    case MESH_ADMIN_SET_CHANNEL:
        if (request->payload.channel.index < 0 ||
            (uint32_t)request->payload.channel.index != request->type) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_set_channel_tag;
        admin.set_channel = request->payload.channel;
        break;
    case MESH_ADMIN_SET_TIME:
        if (request->type < MESH_RADIO_CLOCK_MIN_EPOCH) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_set_time_only_tag;
        admin.set_time_only = request->type;
        break;
    case MESH_ADMIN_SET_FAVORITE:
        if (request->type == 0U) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_set_favorite_node_tag;
        admin.set_favorite_node = request->type;
        break;
    case MESH_ADMIN_REMOVE_FAVORITE:
        if (request->type == 0U) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_remove_favorite_node_tag;
        admin.remove_favorite_node = request->type;
        break;
    case MESH_ADMIN_SET_IGNORED:
        if (request->type == 0U) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_set_ignored_node_tag;
        admin.set_ignored_node = request->type;
        break;
    case MESH_ADMIN_REMOVE_IGNORED:
        if (request->type == 0U) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_remove_ignored_node_tag;
        admin.remove_ignored_node = request->type;
        break;
    /* The actions. A negative second count means "cancel a pending reboot" upstream, which is
       not something this client offers, so `type` is unsigned here and a zero delay - act at
       once, before the ack is out - is refused. */
    case MESH_ADMIN_REBOOT:
        if (request->type == 0U || request->type > INT32_MAX) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_reboot_seconds_tag;
        admin.reboot_seconds = (int32_t)request->type;
        break;
    case MESH_ADMIN_SHUTDOWN:
        if (request->type == 0U || request->type > INT32_MAX) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_shutdown_seconds_tag;
        admin.shutdown_seconds = (int32_t)request->type;
        break;
    case MESH_ADMIN_RESET_NODEDB:
        admin.which_payload_variant = meshtastic_AdminMessage_nodedb_reset_tag;
        admin.nodedb_reset = true;
        break;
    /* The reset pair carry an int the firmware only tests for truth; 1 is what the phone
       apps send. */
    case MESH_ADMIN_FACTORY_RESET_CONFIG:
        admin.which_payload_variant = meshtastic_AdminMessage_factory_reset_config_tag;
        admin.factory_reset_config = 1;
        break;
    case MESH_ADMIN_FACTORY_RESET_DEVICE:
        admin.which_payload_variant = meshtastic_AdminMessage_factory_reset_device_tag;
        admin.factory_reset_device = 1;
        break;
    /* A bare bool, and no delay field to carry one: the firmware acks and resets, and the ack
       is the last thing this link will hear. */
    case MESH_ADMIN_ENTER_DFU_MODE:
        admin.which_payload_variant = meshtastic_AdminMessage_enter_dfu_mode_request_tag;
        admin.enter_dfu_mode_request = true;
        break;
    /* The hash is the whole of this verb's safety: the loader flashes only an image that
       hashes to it. An all-zero one is a caller that forgot to fill it in, and the radio would
       take it - and then strand itself in a loader waiting for a file that cannot exist. */
    case MESH_ADMIN_OTA_REQUEST: {
        bool any = false;
        for (size_t i = 0; i < MESH_ADMIN_OTA_HASH_LEN; ++i) {
            any = any || request->payload.ota_hash[i] != 0U;
        }
        if (!any) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_ota_request_tag;
        admin.ota_request.reboot_ota_mode = meshtastic_OTAMode_OTA_BLE;
        admin.ota_request.ota_hash.size = MESH_ADMIN_OTA_HASH_LEN;
        memcpy(admin.ota_request.ota_hash.bytes, request->payload.ota_hash,
               MESH_ADMIN_OTA_HASH_LEN);
        break;
    }
    case MESH_ADMIN_SET_FIXED_POSITION:
        /* A position with no coordinates would set fixed position on and leave the radio
           broadcasting whatever it had before. */
        if (!request->payload.position.has_latitude_i ||
            !request->payload.position.has_longitude_i) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_set_fixed_position_tag;
        admin.set_fixed_position = request->payload.position;
        break;
    case MESH_ADMIN_REMOVE_FIXED_POSITION:
        admin.which_payload_variant = meshtastic_AdminMessage_remove_fixed_position_tag;
        admin.remove_fixed_position = true;
        break;
    /* A call sign is the whole of what makes this legal, so a ham-mode verb without one is
       refused here rather than sent for the firmware to take literally: an empty call sign
       would rename the node to nothing and still turn the primary channel's key off. */
    case MESH_ADMIN_SET_HAM_MODE:
        if (request->payload.ham.call_sign[0] == '\0') {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_set_ham_mode_tag;
        admin.set_ham_mode = request->payload.ham;
        break;
    case MESH_ADMIN_REMOVE_NODE:
        if (request->type == 0U) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_remove_by_nodenum_tag;
        admin.remove_by_nodenum = request->type;
        break;
    case MESH_ADMIN_TOGGLE_MUTED:
        if (request->type == 0U) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_toggle_muted_node_tag;
        admin.toggle_muted_node = request->type;
        break;
    /* The key is the whole reason this verb exists: an entry with none is what the radio would
       build for itself the first time the node transmitted, so a contact without one is a
       NodeDB slot spent saying nothing. `type` carries the node number for the queue's benefit
       and has to agree with the contact it is filed under. */
    case MESH_ADMIN_ADD_CONTACT:
        if (request->payload.contact.node_num == 0U ||
            request->payload.contact.node_num != request->type ||
            !request->payload.contact.has_user ||
            request->payload.contact.user.public_key.size == 0U) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_add_contact_tag;
        admin.add_contact = request->payload.contact;
        break;
    /*
     * One step of the ceremony. Every step after the first names the nonce the radio opened the
     * exchange with: a step carrying the wrong one - or none - is answering an exchange that is
     * not the one in front of the user, which is the failure the nonce is there to prevent, so
     * it is refused here rather than sent for the firmware to ignore.
     */
    case MESH_ADMIN_KEY_VERIFICATION:
        if (request->payload.key_verification.remote_nodenum == 0U ||
            request->payload.key_verification.remote_nodenum != request->type) {
            return -EINVAL;
        }
        if (request->payload.key_verification.message_type !=
                meshtastic_KeyVerificationAdmin_MessageType_INITIATE_VERIFICATION &&
            request->payload.key_verification.nonce == 0U) {
            return -EINVAL;
        }
        admin.which_payload_variant = meshtastic_AdminMessage_key_verification_tag;
        admin.key_verification = request->payload.key_verification;
        break;
    case MESH_ADMIN_GET_CONNECTION_STATUS:
        admin.which_payload_variant =
            meshtastic_AdminMessage_get_device_connection_status_request_tag;
        admin.get_device_connection_status_request = true;
        break;
    case MESH_ADMIN_GET_UI_CONFIG:
        admin.which_payload_variant = meshtastic_AdminMessage_get_ui_config_request_tag;
        admin.get_ui_config_request = true;
        break;
    case MESH_ADMIN_SET_UI_CONFIG:
        admin.which_payload_variant = meshtastic_AdminMessage_store_ui_config_tag;
        admin.store_ui_config = request->payload.ui_config;
        break;
    case MESH_ADMIN_GET_CANNED_MESSAGES:
        admin.which_payload_variant =
            meshtastic_AdminMessage_get_canned_message_module_messages_request_tag;
        admin.get_canned_message_module_messages_request = true;
        break;
    case MESH_ADMIN_SET_CANNED_MESSAGES:
        admin.which_payload_variant =
            meshtastic_AdminMessage_set_canned_message_module_messages_tag;
        inkwell_str_copy(admin.set_canned_message_module_messages,
                         sizeof admin.set_canned_message_module_messages, request->payload.text);
        break;
    case MESH_ADMIN_GET_RINGTONE:
        admin.which_payload_variant = meshtastic_AdminMessage_get_ringtone_request_tag;
        admin.get_ringtone_request = true;
        break;
    case MESH_ADMIN_BACKUP_PREFERENCES:
        admin.which_payload_variant = meshtastic_AdminMessage_backup_preferences_tag;
        admin.backup_preferences = (meshtastic_AdminMessage_BackupLocation)request->type;
        break;
    case MESH_ADMIN_RESTORE_PREFERENCES:
        admin.which_payload_variant = meshtastic_AdminMessage_restore_preferences_tag;
        admin.restore_preferences = (meshtastic_AdminMessage_BackupLocation)request->type;
        break;
    case MESH_ADMIN_REMOVE_BACKUP_PREFERENCES:
        admin.which_payload_variant = meshtastic_AdminMessage_remove_backup_preferences_tag;
        admin.remove_backup_preferences = (meshtastic_AdminMessage_BackupLocation)request->type;
        break;
    default:
        return -EINVAL;
    }
    if (settings->has_session_passkey && settings->session_passkey_len > 0U) {
        admin.session_passkey.size = (pb_size_t)settings->session_passkey_len;
        memcpy(admin.session_passkey.bytes, settings->session_passkey,
               settings->session_passkey_len);
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *packet = &to_radio.packet;
    /* Addressed to ourselves: the firmware handles it locally and never puts it on the air.
       `from` stays unset, as for text messages. A request carrying a `dest` is readdressed
       below, which is the one thing that puts an AdminMessage on the mesh. */
    packet->to = request->my_node;
    packet->id = request->packet_id;
    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    /* A get is answered with the data; a set with a Routing ack we correlate by id. */
    packet->decoded.want_response = true;

    if (request->dest != 0U) {
        /*
         * Remote administration: the same AdminMessage, addressed to somebody else's radio and
         * put on the air.
         *
         * `pki_encrypted` with the node's key in `public_key` is what the phone apps and the
         * Python CLI send, and the division of labour is the same one every other packet here
         * has - the client says what it wants and the firmware does the cryptography. The
         * firmware seals the payload to that key, so the AdminMessage is readable only by that
         * node and the far end can tell which key signed it; without this it would go out under
         * the channel key, which is a key every node on the channel holds.
         *
         * Refused rather than sent in the clear when the key we hold is not this destination's.
         * A request that fell back would be an admin verb every node on the channel could read,
         * sent by a client that believed it was doing the opposite.
         */
        if (request->dest != settings->admin_dest ||
            settings->admin_dest_key_len != MESH_ADMIN_PUBLIC_KEY_LEN) {
            inkwell_log_warn("admin", "No public key for admin destination 0x%08x", request->dest);
            return -EINVAL;
        }
        packet->to = request->dest;
        packet->pki_encrypted = true;
        packet->public_key.size = (pb_size_t)MESH_ADMIN_PUBLIC_KEY_LEN;
        memcpy(packet->public_key.bytes, settings->admin_dest_key, MESH_ADMIN_PUBLIC_KEY_LEN);
        /*
         * And deliberately **no** `want_ack`, which is the one thing a packet crossing a mesh
         * looks like it ought to have.
         *
         * The reply is the acknowledgement here, the same rule request_position and
         * request_telemetry are written to (see mesh_session_request_on_port). But this queue
         * has a second reason, and it is the stronger one: the whole correlation model is *one*
         * reply per request, quoting our packet id, releasing the one request in flight.
         * `want_ack` adds a second - the firmware reports a delivery ack for a packet we
         * originated as a ROUTING_APP packet quoting that same id - and
         * mesh_radio_settings_ingest_routing() cannot tell it from the answer. It would arrive
         * first, being generated a hop away rather than at the far end, and it would release
         * the queue before the AdminMessage landed: the get's response would then match no
         * pending request and be dropped, and a set would be recorded as saved by its delivery
         * before the firmware's own ADMIN_BAD_SESSION_KEY had a chance to say otherwise.
         *
         * So the retransmissions are given up on purpose, and what stands in for them is the
         * minute above and a refresh the user can press again.
         */
    }

    pb_ostream_t payload =
        pb_ostream_from_buffer(packet->decoded.payload.bytes, sizeof packet->decoded.payload.bytes);
    if (!pb_encode(&payload, meshtastic_AdminMessage_fields, &admin)) {
        inkwell_log_error("admin", "Failed to encode AdminMessage: %s", PB_GET_ERROR(&payload));
        return -EIO;
    }
    packet->decoded.payload.size = (pb_size_t)payload.bytes_written;

    pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
        inkwell_log_error("admin", "Failed to encode admin ToRadio: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }
    *written = stream.bytes_written;
    return 0;
}

/* ---- fetch queue -------------------------------------------------------------------------- */

/*
 * Whether this queue already holds this request *for this destination*.
 *
 * The destination is part of what makes two requests the same one, and not only for tidiness:
 * the passkey refresh in front of every write and every action is a get_owner, one node's
 * passkey is no use to another, and folding a refresh aimed at the admin target into one
 * already queued for our own radio would hand the target a key it will reject.
 */
static bool mesh_radio_settings_queued(const struct mesh_radio_settings *settings, uint32_t dest,
                                       enum mesh_admin_request_kind kind, uint32_t type) {
    for (size_t i = 0; i < settings->queue_len; ++i) {
        const struct mesh_admin_request *entry =
            &settings->queue[(settings->queue_head + i) % MESH_RADIO_SETTINGS_FETCH_MAX];
        if (entry->dest == dest && entry->kind == kind && entry->type == type) {
            return true;
        }
    }
    return false;
}

/*
 * Appends without the deduplication enqueue() does.
 *
 * For the one case where a read queued *earlier* is not a substitute: a read-back exists to
 * observe what a write or an action changed, so folding it into a request already sitting
 * ahead of that action answers with the value the action replaced.
 *
 * Only queue_ham_mode() uses it. queue_write() deliberately does not - the test on set_owner
 * pins that its passkey refresh and its read-back are one request - and changing that is a
 * decision about a shipped mechanism rather than a line in this one.
 */
static size_t mesh_radio_settings_append(struct mesh_radio_settings *settings, uint32_t dest,
                                         enum mesh_admin_request_kind kind, uint32_t type) {
    if (settings->queue_len >= MESH_RADIO_SETTINGS_FETCH_MAX) {
        return 0U;
    }
    struct mesh_admin_request *slot =
        &settings
             ->queue[(settings->queue_head + settings->queue_len) % MESH_RADIO_SETTINGS_FETCH_MAX];
    memset(slot, 0, sizeof *slot);
    slot->dest = dest;
    slot->kind = kind;
    slot->type = type;
    settings->queue_len += 1U;
    return 1U;
}

/*
 * `dest` is the one argument here that is never guessed at: a caller passes `settings->admin_dest`
 * when the request is one the Settings tab makes about a radio's configuration, and 0 when it is
 * about the radio on the end of the link whatever that tab is pointed at. Which of the two each
 * queue call is, is stated where it is made.
 */
static size_t mesh_radio_settings_enqueue(struct mesh_radio_settings *settings, uint32_t dest,
                                          enum mesh_admin_request_kind kind, uint32_t type) {
    if (settings->queue_len >= MESH_RADIO_SETTINGS_FETCH_MAX ||
        mesh_radio_settings_queued(settings, dest, kind, type)) {
        return 0U;
    }
    struct mesh_admin_request *slot =
        &settings
             ->queue[(settings->queue_head + settings->queue_len) % MESH_RADIO_SETTINGS_FETCH_MAX];
    memset(slot, 0, sizeof *slot);
    slot->dest = dest;
    slot->kind = kind;
    slot->type = type;
    settings->queue_len += 1U;
    return 1U;
}

int mesh_radio_settings_queue_write(struct mesh_radio_settings *settings,
                                    const struct mesh_admin_request *write) {
    if (settings == NULL || write == NULL || !mesh_admin_request_is_write(write->kind)) {
        return -EINVAL;
    }
    enum mesh_admin_request_kind readback = MESH_ADMIN_GET_OWNER;
    switch (write->kind) {
    case MESH_ADMIN_SET_OWNER:
        readback = MESH_ADMIN_GET_OWNER;
        break;
    case MESH_ADMIN_SET_CONFIG:
        if (write->payload.config.which_payload_variant == 0U) {
            return -EINVAL;
        }
        readback = MESH_ADMIN_GET_CONFIG;
        break;
    case MESH_ADMIN_SET_MODULE_CONFIG:
        if (write->payload.module_config.which_payload_variant == 0U) {
            return -EINVAL;
        }
        readback = MESH_ADMIN_GET_MODULE_CONFIG;
        break;
    case MESH_ADMIN_SET_CHANNEL:
        if (write->type >= MESH_RADIO_SETTINGS_MAX_CHANNELS ||
            write->payload.channel.index != (int8_t)write->type) {
            return -EINVAL;
        }
        readback = MESH_ADMIN_GET_CHANNEL;
        break;
    case MESH_ADMIN_SET_UI_CONFIG:
        readback = MESH_ADMIN_GET_UI_CONFIG;
        break;
    case MESH_ADMIN_SET_CANNED_MESSAGES:
        readback = MESH_ADMIN_GET_CANNED_MESSAGES;
        break;
    case MESH_ADMIN_SET_FIXED_POSITION:
    case MESH_ADMIN_REMOVE_FIXED_POSITION:
        /* The firmware sets `position.fixed_position` itself, so the section this did not
           write is exactly the one that has to be re-read. */
        if (write->type != (uint32_t)meshtastic_AdminMessage_ConfigType_POSITION_CONFIG) {
            return -EINVAL;
        }
        readback = MESH_ADMIN_GET_CONFIG;
        break;
    default:
        return -EINVAL;
    }
    /* Passkey refresh (unless one is already on its way), the write, the read-back. Writes
       are never deduplicated: two saves of one section both carry a full struct and the
       later one wins, which is what the user asked for. */
    const size_t needed =
        (mesh_radio_settings_queued(settings, settings->admin_dest, MESH_ADMIN_GET_OWNER, 0U)
             ? 0U
             : 1U) +
        1U +
        (mesh_radio_settings_queued(settings, settings->admin_dest, readback, write->type) ? 0U
                                                                                           : 1U);
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added =
        mesh_radio_settings_enqueue(settings, settings->admin_dest, MESH_ADMIN_GET_OWNER, 0U);
    struct mesh_admin_request *slot =
        &settings
             ->queue[(settings->queue_head + settings->queue_len) % MESH_RADIO_SETTINGS_FETCH_MAX];
    *slot = *write;
    /* The caller hands over a kind and a payload; where it goes is this queue's business, so
       the destination is stamped here rather than read off whatever the caller left in it. */
    slot->dest = settings->admin_dest;
    slot->my_node = 0U;
    slot->packet_id = 0U;
    settings->queue_len += 1U;
    added += 1U;
    added += mesh_radio_settings_enqueue(settings, settings->admin_dest, readback, write->type);
    return (int)added;
}

/*
 * The NodeDB verbs all have one shape: a get_owner first, for a passkey the firmware will
 * still accept, then the verb itself. None of them has a get_* to read back with - the flags
 * come home with that node's next NodeInfo, which on a quiet mesh is hours away - so the
 * caller keeps its own copy in step. The node id rides in `type`, which is what keeps two
 * different nodes from looking like a duplicate of one request.
 */
static int mesh_radio_settings_queue_node_op(struct mesh_radio_settings *settings,
                                             enum mesh_admin_request_kind kind, uint32_t node_id) {
    if (settings == NULL || node_id == 0U) {
        return -EINVAL;
    }
    const size_t needed =
        (mesh_radio_settings_queued(settings, 0U, MESH_ADMIN_GET_OWNER, 0U) ? 0U : 1U) +
        (mesh_radio_settings_queued(settings, 0U, kind, node_id) ? 0U : 1U);
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added = mesh_radio_settings_enqueue(settings, 0U, MESH_ADMIN_GET_OWNER, 0U);
    added += mesh_radio_settings_enqueue(settings, 0U, kind, node_id);
    return (int)added;
}

int mesh_radio_settings_queue_ignored(struct mesh_radio_settings *settings, uint32_t node_id,
                                      bool ignored) {
    return mesh_radio_settings_queue_node_op(
        settings, ignored ? MESH_ADMIN_SET_IGNORED : MESH_ADMIN_REMOVE_IGNORED, node_id);
}

int mesh_radio_settings_queue_favorite(struct mesh_radio_settings *settings, uint32_t node_id,
                                       bool favorite) {
    return mesh_radio_settings_queue_node_op(
        settings, favorite ? MESH_ADMIN_SET_FAVORITE : MESH_ADMIN_REMOVE_FAVORITE, node_id);
}

int mesh_radio_settings_queue_remove_node(struct mesh_radio_settings *settings, uint32_t node_id) {
    return mesh_radio_settings_queue_node_op(settings, MESH_ADMIN_REMOVE_NODE, node_id);
}

int mesh_radio_settings_queue_toggle_muted(struct mesh_radio_settings *settings, uint32_t node_id) {
    return mesh_radio_settings_queue_node_op(settings, MESH_ADMIN_TOGGLE_MUTED, node_id);
}

/*
 * The NodeDB shape once more, and the ota_request trick for getting a payload into the slot
 * enqueue() is about to take: the node id rides in `type` so two contacts are two requests, and
 * the contact itself is copied in after the slot has been zeroed.
 *
 * Deduplicated rather than refused, unlike an ota_request. Two presses on one node's row are
 * one contact either way - the second carries the same key off the same roster record - so
 * folding them costs nothing, while an ota_request's two presses can name two different images.
 */
int mesh_radio_settings_queue_contact(struct mesh_radio_settings *settings,
                                      const meshtastic_SharedContact *contact) {
    if (settings == NULL || contact == NULL || contact->node_num == 0U) {
        return -EINVAL;
    }
    if (!contact->has_user || contact->user.public_key.size == 0U) {
        return -EINVAL;
    }
    const size_t needed =
        (mesh_radio_settings_queued(settings, 0U, MESH_ADMIN_GET_OWNER, 0U) ? 0U : 1U) +
        (mesh_radio_settings_queued(settings, 0U, MESH_ADMIN_ADD_CONTACT, contact->node_num) ? 0U
                                                                                             : 1U);
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added = mesh_radio_settings_enqueue(settings, 0U, MESH_ADMIN_GET_OWNER, 0U);
    struct mesh_admin_request *const slot =
        &settings
             ->queue[(settings->queue_head + settings->queue_len) % MESH_RADIO_SETTINGS_FETCH_MAX];
    if (mesh_radio_settings_enqueue(settings, 0U, MESH_ADMIN_ADD_CONTACT, contact->node_num) ==
        1U) {
        slot->payload.contact = *contact;
        added += 1U;
    }
    return (int)added;
}

/*
 * A ceremony step, and the one queue entry in this file that does *not* go through
 * mesh_radio_settings_enqueue().
 *
 * That helper folds a request into one already queued for the same kind and type, which is
 * right for every other verb here - two presses of "pin this node" are one thing to say - and
 * wrong for this one. The four steps are a *sequence*: an INITIATE and the DO_VERIFY that
 * eventually answers it are both addressed to the same node, so folding on the node id would
 * silently drop whichever arrived second, and the ceremony would stall with nobody able to see
 * why. So the slot is filled directly, the way a section write's is.
 *
 * What is deduplicated is a step against *itself* - the same message type for the same node,
 * still waiting to go out - which is a double press and nothing else. That is refused with
 * -EBUSY rather than folded, so the caller can say so instead of reporting a success that sent
 * nothing.
 */
static bool mesh_radio_settings_step_queued(const struct mesh_radio_settings *settings,
                                            uint32_t message_type, uint32_t node_id) {
    for (size_t i = 0; i < settings->queue_len; ++i) {
        const struct mesh_admin_request *const entry =
            &settings->queue[(settings->queue_head + i) % MESH_RADIO_SETTINGS_FETCH_MAX];
        if (entry->kind == MESH_ADMIN_KEY_VERIFICATION && entry->type == node_id &&
            (uint32_t)entry->payload.key_verification.message_type == message_type) {
            return true;
        }
    }
    return false;
}

/*
 * Drops every ceremony step for `node_id` that has not gone out yet.
 *
 * The one caller is the user standing a ceremony down before the radio has answered: the
 * INITIATE may still be sitting in the queue, and sending it would start on the wire exactly
 * what the user just stopped. A request the queue no longer holds has already been handed to
 * the transport (mesh_radio_settings_next_request pops), so there is nothing here to catch it
 * and nothing this can do about it - which is fine, because the radio will then open an
 * exchange the user can stand down properly, with a nonce to do it with.
 *
 * Compacts the ring in place rather than marking entries dead: the queue is walked by three
 * other functions that all assume every slot between head and head+len is a real request, and a
 * tombstone would be a fourth rule for each of them to remember.
 */
size_t mesh_radio_settings_cancel_key_verification(struct mesh_radio_settings *settings,
                                                   uint32_t node_id) {
    if (settings == NULL || node_id == 0U) {
        return 0U;
    }
    size_t kept = 0U;
    size_t dropped = 0U;
    for (size_t i = 0; i < settings->queue_len; ++i) {
        const size_t from = (settings->queue_head + i) % MESH_RADIO_SETTINGS_FETCH_MAX;
        const struct mesh_admin_request *const entry = &settings->queue[from];
        if (entry->kind == MESH_ADMIN_KEY_VERIFICATION && entry->type == node_id) {
            dropped += 1U;
            continue;
        }
        const size_t to = (settings->queue_head + kept) % MESH_RADIO_SETTINGS_FETCH_MAX;
        if (to != from) {
            settings->queue[to] = settings->queue[from];
        }
        kept += 1U;
    }
    if (dropped > 0U) {
        /* Zero the tail so a stale payload cannot be read back through a slot the next enqueue
           has not filled yet - enqueue() memsets its own, but the direct writers do not. */
        for (size_t i = kept; i < settings->queue_len; ++i) {
            const size_t slot = (settings->queue_head + i) % MESH_RADIO_SETTINGS_FETCH_MAX;
            memset(&settings->queue[slot], 0, sizeof settings->queue[slot]);
        }
        settings->queue_len = kept;
        inkwell_log_info("admin", "Dropped %zu unsent verification step(s) for 0x%08x", dropped,
                         node_id);
    }
    return dropped;
}

int mesh_radio_settings_queue_key_verification(struct mesh_radio_settings *settings,
                                               uint32_t message_type, uint32_t node_id,
                                               uint64_t nonce, bool has_number, uint32_t number) {
    if (settings == NULL || node_id == 0U) {
        return -EINVAL;
    }
    if (message_type > (uint32_t)meshtastic_KeyVerificationAdmin_MessageType_DO_NOT_VERIFY) {
        return -EINVAL;
    }
    if (message_type !=
            (uint32_t)meshtastic_KeyVerificationAdmin_MessageType_INITIATE_VERIFICATION &&
        nonce == 0U) {
        return -EINVAL;
    }
    if (mesh_radio_settings_step_queued(settings, message_type, node_id)) {
        return -EBUSY;
    }
    const size_t needed =
        (mesh_radio_settings_queued(settings, 0U, MESH_ADMIN_GET_OWNER, 0U) ? 0U : 1U) + 1U;
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added = mesh_radio_settings_enqueue(settings, 0U, MESH_ADMIN_GET_OWNER, 0U);
    struct mesh_admin_request *const slot =
        &settings
             ->queue[(settings->queue_head + settings->queue_len) % MESH_RADIO_SETTINGS_FETCH_MAX];
    memset(slot, 0, sizeof *slot);
    slot->kind = MESH_ADMIN_KEY_VERIFICATION;
    slot->type = node_id;
    slot->payload.key_verification.message_type =
        (meshtastic_KeyVerificationAdmin_MessageType)message_type;
    slot->payload.key_verification.remote_nodenum = node_id;
    slot->payload.key_verification.nonce = nonce;
    /* Only the step that carries digits carries the field: the firmware reads it for
       PROVIDE_SECURITY_NUMBER and a 0 sent on any other step would be a number claimed. */
    slot->payload.key_verification.has_security_number = has_number;
    slot->payload.key_verification.security_number = has_number ? number : 0U;
    settings->queue_len += 1U;
    added += 1U;
    return (int)added;
}

int mesh_radio_settings_queue_action(struct mesh_radio_settings *settings,
                                     enum mesh_admin_request_kind kind, uint32_t seconds) {
    /* An ota_request and a set_ham_mode are actions with a payload, and this call has nowhere
       to put one: each has a queue call of its own that insists on what it carries. */
    if (settings == NULL || !mesh_admin_request_is_action(kind) || kind == MESH_ADMIN_OTA_REQUEST ||
        kind == MESH_ADMIN_SET_HAM_MODE) {
        return -EINVAL;
    }
    /*
     * The one action that does not travel.
     *
     * Every other action here leaves a radio that comes back: a reboot returns, a shutdown is
     * pressed again at the node, and a reset leaves a node that still speaks Meshtastic. The
     * UF2 bootloader does not - the radio stops being a mesh node and becomes a USB drive
     * waiting for a file - so sent over the air this is a verb that takes a node off the mesh
     * and puts the only way back at the far end of a walk. It stays a thing you do to the radio
     * in your hand.
     */
    if (settings->admin_dest != 0U && kind == MESH_ADMIN_ENTER_DFU_MODE) {
        return -EINVAL;
    }
    /* Only the reboot and the shutdown have a delay; the resets carry nothing, and pinning
       their `type` at zero keeps two presses of one reset a single queued request. */
    const uint32_t type = (kind == MESH_ADMIN_REBOOT || kind == MESH_ADMIN_SHUTDOWN) ? seconds : 0U;
    if ((kind == MESH_ADMIN_REBOOT || kind == MESH_ADMIN_SHUTDOWN) && type == 0U) {
        return -EINVAL;
    }
    /* The NodeDB verbs' shape once more, minus the node id: a passkey the firmware will still
       accept, then the action. There is nothing to read back - a rebooting radio has no state
       to re-read, and a factory-reset one has none we would recognise. */
    const size_t needed =
        (mesh_radio_settings_queued(settings, settings->admin_dest, MESH_ADMIN_GET_OWNER, 0U)
             ? 0U
             : 1U) +
        (mesh_radio_settings_queued(settings, settings->admin_dest, kind, type) ? 0U : 1U);
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added =
        mesh_radio_settings_enqueue(settings, settings->admin_dest, MESH_ADMIN_GET_OWNER, 0U);
    added += mesh_radio_settings_enqueue(settings, settings->admin_dest, kind, type);
    return (int)added;
}

int mesh_radio_settings_queue_ota(struct mesh_radio_settings *settings,
                                  const uint8_t hash[MESH_ADMIN_OTA_HASH_LEN]) {
    if (settings == NULL || hash == NULL) {
        return -EINVAL;
    }
    bool any = false;
    for (size_t i = 0; i < MESH_ADMIN_OTA_HASH_LEN; ++i) {
        any = any || hash[i] != 0U;
    }
    if (!any) {
        return -EINVAL;
    }
    /* Deduplicated like any action - but refused rather than folded, because the payload is
       the point: two presses naming two images must not become one request naming whichever
       came first. */
    if (mesh_radio_settings_queued(settings, 0U, MESH_ADMIN_OTA_REQUEST, 0U)) {
        return -EBUSY;
    }
    const size_t needed =
        (mesh_radio_settings_queued(settings, 0U, MESH_ADMIN_GET_OWNER, 0U) ? 0U : 1U) + 1U;
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added = mesh_radio_settings_enqueue(settings, 0U, MESH_ADMIN_GET_OWNER, 0U);
    /* The slot enqueue() is about to fill, so the hash can go in after it has been zeroed. */
    struct mesh_admin_request *const slot =
        &settings
             ->queue[(settings->queue_head + settings->queue_len) % MESH_RADIO_SETTINGS_FETCH_MAX];
    if (mesh_radio_settings_enqueue(settings, 0U, MESH_ADMIN_OTA_REQUEST, 0U) == 1U) {
        memcpy(slot->payload.ota_hash, hash, MESH_ADMIN_OTA_HASH_LEN);
        added += 1U;
    }
    return (int)added;
}

int mesh_radio_settings_queue_ham_mode(struct mesh_radio_settings *settings,
                                       const meshtastic_HamParameters *ham) {
    if (settings == NULL || ham == NULL || ham->call_sign[0] == '\0') {
        return -EINVAL;
    }
    /* Refused rather than folded while one is in flight, the reason queue_ota() gives: the
       payload is the point, and two presses naming two call signs must not become one request
       naming whichever was pressed first. */
    if (mesh_radio_settings_queued(settings, settings->admin_dest, MESH_ADMIN_SET_HAM_MODE, 0U)) {
        return -EBUSY;
    }
    /* The passkey refresh, the verb, and an owner read *after* it - three, or two when a
       passkey refresh is already on its way. */
    const size_t needed =
        (mesh_radio_settings_queued(settings, settings->admin_dest, MESH_ADMIN_GET_OWNER, 0U)
             ? 0U
             : 1U) +
        2U;
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added =
        mesh_radio_settings_enqueue(settings, settings->admin_dest, MESH_ADMIN_GET_OWNER, 0U);
    struct mesh_admin_request *const slot =
        &settings
             ->queue[(settings->queue_head + settings->queue_len) % MESH_RADIO_SETTINGS_FETCH_MAX];
    if (mesh_radio_settings_enqueue(settings, settings->admin_dest, MESH_ADMIN_SET_HAM_MODE, 0U) ==
        1U) {
        slot->payload.ham = *ham;
        added += 1U;
    }
    /*
     * The owner read this action is actually for, appended rather than enqueued.
     *
     * What set_ham_mode changes about the owner - the long name becomes the call sign and the
     * licensed flag goes on - is the half of it a caller's refresh cannot ask for: every path
     * into this queue puts a get_owner in front for the passkey, and enqueue() would fold the
     * read-back into that one, so the only owner reply would describe the node as it was
     * before the switch. The rest of what moved (LoRa, the primary channel) the caller's
     * refresh picks up behind this, because those reads are not already queued.
     */
    added += mesh_radio_settings_append(settings, settings->admin_dest, MESH_ADMIN_GET_OWNER, 0U);
    return (int)added;
}

int mesh_radio_settings_queue_time(struct mesh_radio_settings *settings, uint32_t epoch) {
    if (settings == NULL) {
        return -EINVAL;
    }
    if (epoch < MESH_RADIO_CLOCK_MIN_EPOCH) {
        return -EINVAL;
    }
    /* Same shape as a write minus the read-back: a get_owner first, because the firmware
       rejects a set_* whose session passkey has aged out and the one we hold may be minutes
       old. The epoch rides in `type`, so a second push with a different time is not mistaken
       for a duplicate of the first. */
    const size_t needed =
        (mesh_radio_settings_queued(settings, 0U, MESH_ADMIN_GET_OWNER, 0U) ? 0U : 1U) +
        (mesh_radio_settings_queued(settings, 0U, MESH_ADMIN_SET_TIME, epoch) ? 0U : 1U);
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added = mesh_radio_settings_enqueue(settings, 0U, MESH_ADMIN_GET_OWNER, 0U);
    added += mesh_radio_settings_enqueue(settings, 0U, MESH_ADMIN_SET_TIME, epoch);
    return (int)added;
}

bool mesh_radio_settings_write_pending(const struct mesh_radio_settings *settings) {
    if (settings == NULL) {
        return false;
    }
    if (settings->pending_request_id != 0U && settings->pending_is_write) {
        return true;
    }
    for (size_t i = 0; i < settings->queue_len; ++i) {
        const struct mesh_admin_request *entry =
            &settings->queue[(settings->queue_head + i) % MESH_RADIO_SETTINGS_FETCH_MAX];
        if (mesh_admin_request_is_write(entry->kind)) {
            return true;
        }
    }
    return false;
}

void mesh_radio_settings_mark_unsent(struct mesh_radio_settings *settings,
                                     const struct mesh_admin_request *request, int error) {
    if (settings == NULL || request == NULL || !mesh_admin_request_is_write(request->kind)) {
        return;
    }
    mesh_radio_settings_record_write_result(settings, error != 0 ? (int32_t)error : -EIO);
}

size_t mesh_radio_settings_queue_probe(struct mesh_radio_settings *settings) {
    if (settings == NULL) {
        return 0U;
    }
    size_t added = 0U;
    added +=
        mesh_radio_settings_enqueue(settings, settings->admin_dest, MESH_ADMIN_GET_METADATA, 0U);
    added += mesh_radio_settings_enqueue(settings, settings->admin_dest, MESH_ADMIN_GET_OWNER, 0U);
    return added;
}

size_t mesh_radio_settings_queue_all(struct mesh_radio_settings *settings) {
    if (settings == NULL) {
        return 0U;
    }
    static const uint32_t k_config_types[] = {
        meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG,
        meshtastic_AdminMessage_ConfigType_LORA_CONFIG,
        meshtastic_AdminMessage_ConfigType_BLUETOOTH_CONFIG,
        meshtastic_AdminMessage_ConfigType_DISPLAY_CONFIG,
        meshtastic_AdminMessage_ConfigType_SECURITY_CONFIG,
        meshtastic_AdminMessage_ConfigType_POSITION_CONFIG,
        meshtastic_AdminMessage_ConfigType_POWER_CONFIG,
        meshtastic_AdminMessage_ConfigType_NETWORK_CONFIG,
    };

    size_t added = mesh_radio_settings_queue_probe(settings);
    for (size_t i = 0; i < sizeof k_config_types / sizeof k_config_types[0]; ++i) {
        added += mesh_radio_settings_enqueue(settings, settings->admin_dest, MESH_ADMIN_GET_CONFIG,
                                             k_config_types[i]);
    }
    /* One per module, from the table rather than a list kept beside it: a module whose row
       exists is refreshed, and one that is not kept is not asked for. */
    for (size_t i = 0; i < mesh_radio_module_count(); ++i) {
        added += mesh_radio_settings_enqueue(settings, settings->admin_dest,
                                             MESH_ADMIN_GET_MODULE_CONFIG,
                                             mesh_radio_module_at(i)->admin_type);
    }
    /* The four that are neither a Config nor a ModuleConfig. A radio too old to know a verb
       answers nothing at all rather than erroring, and the queue's own timeout moves past it -
       which is why they can be asked for unconditionally. */
    added += mesh_radio_settings_enqueue(settings, settings->admin_dest,
                                         MESH_ADMIN_GET_CONNECTION_STATUS, 0U);
    added +=
        mesh_radio_settings_enqueue(settings, settings->admin_dest, MESH_ADMIN_GET_UI_CONFIG, 0U);
    added += mesh_radio_settings_enqueue(settings, settings->admin_dest,
                                         MESH_ADMIN_GET_CANNED_MESSAGES, 0U);
    added +=
        mesh_radio_settings_enqueue(settings, settings->admin_dest, MESH_ADMIN_GET_RINGTONE, 0U);
    for (uint32_t slot = 0; slot < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++slot) {
        added += mesh_radio_settings_enqueue(settings, settings->admin_dest, MESH_ADMIN_GET_CHANNEL,
                                             slot);
    }
    return added;
}

bool mesh_radio_settings_busy(const struct mesh_radio_settings *settings) {
    return settings != NULL && settings->pending_request_id != 0U;
}

void mesh_radio_settings_note_heard(struct mesh_radio_settings *settings) {
    if (settings != NULL) {
        settings->local_silence = 0U;
    }
}

bool mesh_radio_settings_next_request(struct mesh_radio_settings *settings, uint64_t now_ms,
                                      struct mesh_admin_request *out) {
    if (settings == NULL || out == NULL) {
        return false;
    }
    if (settings->pending_request_id != 0U) {
        /* Which deadline applies is a property of where the request went, not of where the tab
           is pointed now: a local clock push queued behind a remote refresh is still a local
           round trip and should not be given a minute to make it. */
        const uint32_t deadline = settings->pending_dest != 0U
                                      ? MESH_RADIO_SETTINGS_REMOTE_REPLY_TIMEOUT_MS
                                      : MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS;
        if (now_ms - settings->pending_sent_at_ms < deadline) {
            return false;
        }
        inkwell_log_warn("admin", "No reply to admin request %u after %u ms; moving on",
                         settings->pending_request_id, deadline);
        settings->timeouts += 1U;
        if (settings->pending_dest != 0U) {
            settings->remote_silence += 1U;
        } else {
            settings->local_silence += 1U;
        }
        if (settings->pending_is_write) {
            mesh_radio_settings_record_write_result(settings, MESH_RADIO_SETTINGS_WRITE_TIMEOUT);
        }
        const bool give_up = settings->remote_silence >= MESH_RADIO_SETTINGS_REMOTE_GIVE_UP;
        const bool link_silent = settings->local_silence >= MESH_RADIO_SETTINGS_LOCAL_GIVE_UP;
        settings->pending_request_id = 0U;
        settings->pending_sent_at_ms = 0U;
        settings->pending_is_write = false;
        settings->pending_dest = 0U;
        /*
         * Enough silence from a node over the air and the rest of the queue is dropped.
         *
         * A refresh is nearly thirty requests and a remote one waits a minute for each, so a
         * node that is out of range or is not letting us administer it would leave the tab
         * looking busy for half an hour and then show what it showed at the start. Stopping
         * says the same thing sooner, and the refresh row is one press away when the user has
         * moved or the path has come back.
         */
        if (give_up) {
            inkwell_log_warn("admin", "Node 0x%08x has not answered %u admin requests; giving up",
                             settings->admin_dest, MESH_RADIO_SETTINGS_REMOTE_GIVE_UP);
            memset(settings->queue, 0, sizeof settings->queue);
            settings->queue_head = 0U;
            settings->queue_len = 0U;
            settings->remote_silence = 0U;
            return false;
        }
        /* The radio we are attached to, not one across the mesh: nothing left in the queue can
           be answered either, and the link is handed back for the transport to drop. */
        if (link_silent) {
            inkwell_log_warn("admin",
                             "The radio has not answered %u admin requests and sent nothing else; "
                             "calling the link dead",
                             MESH_RADIO_SETTINGS_LOCAL_GIVE_UP);
            memset(settings->queue, 0, sizeof settings->queue);
            settings->queue_head = 0U;
            settings->queue_len = 0U;
            settings->local_silence = 0U;
            settings->link_silent = true;
            return false;
        }
    }
    if (settings->queue_len == 0U) {
        return false;
    }
    *out = settings->queue[settings->queue_head];
    settings->queue_head = (settings->queue_head + 1U) % MESH_RADIO_SETTINGS_FETCH_MAX;
    settings->queue_len -= 1U;
    /* Remember what kind went out, and where, so the reply (or its absence) is accounted
       correctly. The caller's mark_sent() confirms it actually left. */
    settings->pending_is_write = mesh_admin_request_is_write(out->kind);
    settings->pending_dest = out->dest;
    return true;
}

void mesh_radio_settings_mark_sent(struct mesh_radio_settings *settings, uint32_t packet_id,
                                   uint64_t now_ms) {
    if (settings == NULL) {
        return;
    }
    settings->pending_request_id = packet_id;
    settings->pending_sent_at_ms = now_ms;
    if (settings->pending_is_write) {
        settings->writes_sent += 1U;
    }
}

/* ---- names -------------------------------------------------------------------------------- */

const char *mesh_radio_role_name(uint32_t role) {
    switch (role) {
    case meshtastic_Config_DeviceConfig_Role_CLIENT:
        return "Client";
    case meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE:
        return "Client Mute";
    case meshtastic_Config_DeviceConfig_Role_ROUTER:
        return "Router";
    case meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT:
        return "Router Client";
    case meshtastic_Config_DeviceConfig_Role_REPEATER:
        return "Repeater";
    case meshtastic_Config_DeviceConfig_Role_TRACKER:
        return "Tracker";
    case meshtastic_Config_DeviceConfig_Role_SENSOR:
        return "Sensor";
    case meshtastic_Config_DeviceConfig_Role_TAK:
        return "TAK";
    case meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN:
        return "Client Hidden";
    case meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND:
        return "Lost and Found";
    case meshtastic_Config_DeviceConfig_Role_TAK_TRACKER:
        return "TAK Tracker";
    case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
        return "Router Late";
    case meshtastic_Config_DeviceConfig_Role_CLIENT_BASE:
        return "Client Base";
    default:
        return "?";
    }
}

const char *mesh_radio_region_name(uint32_t region) {
    switch (region) {
    case meshtastic_Config_LoRaConfig_RegionCode_UNSET:
        return "Unset";
    case meshtastic_Config_LoRaConfig_RegionCode_US:
        return "US";
    case meshtastic_Config_LoRaConfig_RegionCode_EU_433:
        return "EU 433";
    case meshtastic_Config_LoRaConfig_RegionCode_EU_868:
        return "EU 868";
    case meshtastic_Config_LoRaConfig_RegionCode_CN:
        return "CN";
    case meshtastic_Config_LoRaConfig_RegionCode_JP:
        return "JP";
    case meshtastic_Config_LoRaConfig_RegionCode_ANZ:
        return "ANZ";
    case meshtastic_Config_LoRaConfig_RegionCode_KR:
        return "KR";
    case meshtastic_Config_LoRaConfig_RegionCode_TW:
        return "TW";
    case meshtastic_Config_LoRaConfig_RegionCode_RU:
        return "RU";
    case meshtastic_Config_LoRaConfig_RegionCode_IN:
        return "IN";
    case meshtastic_Config_LoRaConfig_RegionCode_NZ_865:
        return "NZ 865";
    case meshtastic_Config_LoRaConfig_RegionCode_TH:
        return "TH";
    case meshtastic_Config_LoRaConfig_RegionCode_LORA_24:
        return "LoRa 2.4G";
    case meshtastic_Config_LoRaConfig_RegionCode_UA_433:
        return "UA 433";
    case meshtastic_Config_LoRaConfig_RegionCode_UA_868:
        return "UA 868";
    case meshtastic_Config_LoRaConfig_RegionCode_MY_433:
        return "MY 433";
    case meshtastic_Config_LoRaConfig_RegionCode_MY_919:
        return "MY 919";
    case meshtastic_Config_LoRaConfig_RegionCode_SG_923:
        return "SG 923";
    case meshtastic_Config_LoRaConfig_RegionCode_PH_433:
        return "PH 433";
    case meshtastic_Config_LoRaConfig_RegionCode_PH_868:
        return "PH 868";
    case meshtastic_Config_LoRaConfig_RegionCode_PH_915:
        return "PH 915";
    case meshtastic_Config_LoRaConfig_RegionCode_ANZ_433:
        return "ANZ 433";
    case meshtastic_Config_LoRaConfig_RegionCode_KZ_433:
        return "KZ 433";
    case meshtastic_Config_LoRaConfig_RegionCode_KZ_863:
        return "KZ 863";
    case meshtastic_Config_LoRaConfig_RegionCode_NP_865:
        return "NP 865";
    case meshtastic_Config_LoRaConfig_RegionCode_BR_902:
        return "BR 902";
    case meshtastic_Config_LoRaConfig_RegionCode_ITU1_2M:
        return "ITU1 2m";
    case meshtastic_Config_LoRaConfig_RegionCode_ITU2_2M:
        return "ITU2 2m";
    case meshtastic_Config_LoRaConfig_RegionCode_EU_866:
        return "EU 866";
    case meshtastic_Config_LoRaConfig_RegionCode_EU_874:
        return "EU 874";
    case meshtastic_Config_LoRaConfig_RegionCode_EU_917:
        return "EU 917";
    case meshtastic_Config_LoRaConfig_RegionCode_EU_N_868:
        return "EU narrow 868";
    case meshtastic_Config_LoRaConfig_RegionCode_ITU3_2M:
        return "ITU3 2m";
    case meshtastic_Config_LoRaConfig_RegionCode_ITU1_70CM:
        return "ITU1 70cm";
    case meshtastic_Config_LoRaConfig_RegionCode_ITU2_70CM:
        return "ITU2 70cm";
    case meshtastic_Config_LoRaConfig_RegionCode_ITU3_70CM:
        return "ITU3 70cm";
    case meshtastic_Config_LoRaConfig_RegionCode_ITU2_125CM:
        return "ITU2 1.25m";
    default:
        return "?";
    }
}

const char *mesh_radio_modem_preset_name(uint32_t preset) {
    switch (preset) {
    case meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST:
        return "Long Range - Fast";
    case meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW:
        return "Long Range - Slow";
    case meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW:
        return "Very Long - Slow";
    case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW:
        return "Medium Range - Slow";
    case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST:
        return "Medium Range - Fast";
    case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW:
        return "Short Range - Slow";
    case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST:
        return "Short Range - Fast";
    case meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE:
        return "Long Range - Moderate";
    case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO:
        return "Short Range - Turbo";
    case meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO:
        return "Long Range - Turbo";
    case meshtastic_Config_LoRaConfig_ModemPreset_LITE_FAST:
        return "Lite - Fast";
    case meshtastic_Config_LoRaConfig_ModemPreset_LITE_SLOW:
        return "Lite - Slow";
    case meshtastic_Config_LoRaConfig_ModemPreset_NARROW_FAST:
        return "Narrow - Fast";
    case meshtastic_Config_LoRaConfig_ModemPreset_NARROW_SLOW:
        return "Narrow - Slow";
    case meshtastic_Config_LoRaConfig_ModemPreset_TINY_FAST:
        return "Tiny - Fast";
    case meshtastic_Config_LoRaConfig_ModemPreset_TINY_SLOW:
        return "Tiny - Slow";
    case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_TURBO:
        return "Medium Range - Turbo";
    default:
        return "?";
    }
}

/*
 * meshtastic_Language, for the row that shows what language the radio's own screen is in.
 *
 * Here rather than in the UI for the reason the hardware names and the modem presets are: it
 * is a fixed vocabulary belonging to the radio's firmware, and somebody matching the Brick's
 * row against the phone app needs the same word on both screens.
 *
 * The values are the one enum in this client that is not 0..n-1 - they run 0..19 and then jump
 * to 30 and 31 - which is why the two at the end are named by hand and why nothing offers this
 * as a row that steps.
 */
const char *mesh_radio_language_name(uint32_t language) {
    static const char *const k_names[] = {
        "English",   "French",    "German",    "Italian",   "Portuguese", "Spanish", "Swedish",
        "Finnish",   "Polish",    "Turkish",   "Serbian",   "Russian",    "Dutch",   "Greek",
        "Norwegian", "Slovenian", "Ukrainian", "Bulgarian", "Czech",      "Danish",
    };
    if (language < sizeof k_names / sizeof k_names[0]) {
        return k_names[language];
    }
    if (language == 30U) {
        return "Chinese (simplified)";
    }
    if (language == 31U) {
        return "Chinese (traditional)";
    }
    return "?";
}

/* The enum has ~150 boards; these are the ones likely to be paired with a Brick. Anything
   else shows as "model N", which is still enough to look up. */
const char *mesh_radio_hw_model_name(uint32_t model, char *fallback, size_t fallback_len) {
    switch (model) {
    case meshtastic_HardwareModel_UNSET:
        return "unknown";
    case meshtastic_HardwareModel_TLORA_V2_1_1P6:
        return "LILYGO T-LoRa v2.1";
    case meshtastic_HardwareModel_TBEAM:
        return "LILYGO T-Beam";
    case meshtastic_HardwareModel_T_ECHO:
        return "LILYGO T-Echo";
    case meshtastic_HardwareModel_RAK4631:
        return "RAK4631";
    case meshtastic_HardwareModel_LILYGO_TBEAM_S3_CORE:
        return "LILYGO T-Beam S3";
    case meshtastic_HardwareModel_TLORA_T3_S3:
        return "LILYGO T3-S3";
    case meshtastic_HardwareModel_RAK11310:
        return "RAK11310";
    case meshtastic_HardwareModel_STATION_G2:
        return "Station G2";
    case meshtastic_HardwareModel_T_ECHO_PLUS:
        return "LILYGO T-Echo Plus";
    case meshtastic_HardwareModel_PORTDUINO:
        return "Linux native";
    case meshtastic_HardwareModel_HELTEC_V3:
        return "Heltec V3";
    case meshtastic_HardwareModel_HELTEC_WSL_V3:
        return "Heltec WSL V3";
    case meshtastic_HardwareModel_RPI_PICO:
        return "Raspberry Pi Pico";
    case meshtastic_HardwareModel_HELTEC_WIRELESS_TRACKER:
        return "Heltec Wireless Tracker";
    case meshtastic_HardwareModel_HELTEC_WIRELESS_PAPER:
        return "Heltec Wireless Paper";
    case meshtastic_HardwareModel_T_DECK:
        return "LILYGO T-Deck";
    case meshtastic_HardwareModel_T_WATCH_S3:
        return "LILYGO T-Watch S3";
    case meshtastic_HardwareModel_HELTEC_HT62:
        return "Heltec HT62";
    case meshtastic_HardwareModel_NRF52_PROMICRO_DIY:
        return "nRF52 ProMicro DIY";
    case meshtastic_HardwareModel_HELTEC_VISION_MASTER_E290:
        return "Heltec Vision Master E290";
    case meshtastic_HardwareModel_HELTEC_MESH_NODE_T114:
        return "Heltec Mesh Node T114";
    case meshtastic_HardwareModel_SENSECAP_INDICATOR:
        return "SenseCAP Indicator";
    case meshtastic_HardwareModel_TRACKER_T1000_E:
        return "Seeed T1000-E";
    case meshtastic_HardwareModel_RAK3172:
        return "RAK3172";
    case meshtastic_HardwareModel_SEEED_XIAO_S3:
        return "Seeed XIAO S3";
    case meshtastic_HardwareModel_HELTEC_MESH_POCKET:
        return "Heltec Mesh Pocket";
    case meshtastic_HardwareModel_T_DECK_PRO:
        return "LILYGO T-Deck Pro";
    case meshtastic_HardwareModel_HELTEC_V4:
        return "Heltec V4";
    case meshtastic_HardwareModel_PRIVATE_HW:
        return "private hardware";
    default:
        break;
    }
    if (fallback == NULL || fallback_len == 0U) {
        return "?";
    }
    snprintf(fallback, fallback_len, "model %u", (unsigned)model);
    return fallback;
}
