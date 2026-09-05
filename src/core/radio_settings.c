#include "mesh/core/radio_settings.h"

#include "mesh/utils/log.h"
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

bool mesh_radio_settings_loaded(const struct mesh_radio_settings *settings) {
    if (settings == NULL) {
        return false;
    }
    /* The contract is "anything has arrived". The Config sections are still listed by hand -
       there are eight of them and they do not grow - but the modules come from the table, so a
       new one counts here the moment its row exists. Forgetting this list is what made a radio
       read as absent while we held its config. */
    if (settings->has_device || settings->has_position || settings->has_power ||
        settings->has_network || settings->has_display || settings->has_lora ||
        settings->has_bluetooth || settings->has_security || settings->has_owner ||
        settings->has_metadata) {
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

/* ---- admin replies ------------------------------------------------------------------------ */

bool mesh_admin_request_is_write(enum mesh_admin_request_kind kind) {
    /* MESH_ADMIN_SET_TIME is a set_* on the wire but not here: see the header. The two
       fixed-position kinds *are* writes: they are a save the user pressed for, they are acked
       like any other, and the get_config POSITION behind them makes the row show what the
       radio actually kept. */
    return kind == MESH_ADMIN_SET_OWNER || kind == MESH_ADMIN_SET_CONFIG ||
           kind == MESH_ADMIN_SET_MODULE_CONFIG || kind == MESH_ADMIN_SET_CHANNEL ||
           kind == MESH_ADMIN_SET_FIXED_POSITION || kind == MESH_ADMIN_REMOVE_FIXED_POSITION;
}

bool mesh_admin_request_is_action(enum mesh_admin_request_kind kind) {
    return kind == MESH_ADMIN_REBOOT || kind == MESH_ADMIN_SHUTDOWN ||
           kind == MESH_ADMIN_RESET_NODEDB || kind == MESH_ADMIN_FACTORY_RESET_CONFIG ||
           kind == MESH_ADMIN_FACTORY_RESET_DEVICE;
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
    return true;
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
        mesh_log_warn("admin", "Undecodable Routing reply to admin request %u: %s", request_id,
                      PB_GET_ERROR(&stream));
    } else if (routing.which_variant == meshtastic_Routing_error_reason_tag) {
        error = (int32_t)routing.error_reason;
    }
    if (error == 0) {
        mesh_log_info("admin", "Admin request %u acknowledged", request_id);
    } else {
        mesh_log_warn("admin", "Admin request %u rejected: routing error %d", request_id,
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
        mesh_log_warn("admin", "Undecodable AdminMessage (request_id=%u): %s",
                      packet->decoded.request_id, PB_GET_ERROR(&stream));
        mesh_radio_settings_finish_pending(settings, packet->decoded.request_id, 0);
        return 1;
    }

    settings->admin_replies += 1U;
    if (admin.session_passkey.size > 0U) {
        size_t len = admin.session_passkey.size;
        if (len > sizeof settings->session_passkey) {
            len = sizeof settings->session_passkey;
        }
        memcpy(settings->session_passkey, admin.session_passkey.bytes, len);
        settings->session_passkey_len = len;
        settings->has_session_passkey = true;
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
    default:
        break;
    }

    mesh_log_info("admin", "Admin reply: %s (request_id=%u, variant=%u, session passkey %s)", what,
                  packet->decoded.request_id, (unsigned)admin.which_payload_variant,
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
       `from` stays unset, as for text messages. */
    packet->to = request->my_node;
    packet->id = request->packet_id;
    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    /* A get is answered with the data; a set with a Routing ack we correlate by id. */
    packet->decoded.want_response = true;

    pb_ostream_t payload =
        pb_ostream_from_buffer(packet->decoded.payload.bytes, sizeof packet->decoded.payload.bytes);
    if (!pb_encode(&payload, meshtastic_AdminMessage_fields, &admin)) {
        mesh_log_error("admin", "Failed to encode AdminMessage: %s", PB_GET_ERROR(&payload));
        return -EIO;
    }
    packet->decoded.payload.size = (pb_size_t)payload.bytes_written;

    pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
        mesh_log_error("admin", "Failed to encode admin ToRadio: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }
    *written = stream.bytes_written;
    return 0;
}

/* ---- fetch queue -------------------------------------------------------------------------- */

static bool mesh_radio_settings_queued(const struct mesh_radio_settings *settings,
                                       enum mesh_admin_request_kind kind, uint32_t type) {
    for (size_t i = 0; i < settings->queue_len; ++i) {
        const struct mesh_admin_request *entry =
            &settings->queue[(settings->queue_head + i) % MESH_RADIO_SETTINGS_FETCH_MAX];
        if (entry->kind == kind && entry->type == type) {
            return true;
        }
    }
    return false;
}

static size_t mesh_radio_settings_enqueue(struct mesh_radio_settings *settings,
                                          enum mesh_admin_request_kind kind, uint32_t type) {
    if (settings->queue_len >= MESH_RADIO_SETTINGS_FETCH_MAX ||
        mesh_radio_settings_queued(settings, kind, type)) {
        return 0U;
    }
    struct mesh_admin_request *slot =
        &settings
             ->queue[(settings->queue_head + settings->queue_len) % MESH_RADIO_SETTINGS_FETCH_MAX];
    memset(slot, 0, sizeof *slot);
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
        (mesh_radio_settings_queued(settings, MESH_ADMIN_GET_OWNER, 0U) ? 0U : 1U) + 1U +
        (mesh_radio_settings_queued(settings, readback, write->type) ? 0U : 1U);
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added = mesh_radio_settings_enqueue(settings, MESH_ADMIN_GET_OWNER, 0U);
    struct mesh_admin_request *slot =
        &settings
             ->queue[(settings->queue_head + settings->queue_len) % MESH_RADIO_SETTINGS_FETCH_MAX];
    *slot = *write;
    slot->my_node = 0U;
    slot->packet_id = 0U;
    settings->queue_len += 1U;
    added += 1U;
    added += mesh_radio_settings_enqueue(settings, readback, write->type);
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
        (mesh_radio_settings_queued(settings, MESH_ADMIN_GET_OWNER, 0U) ? 0U : 1U) +
        (mesh_radio_settings_queued(settings, kind, node_id) ? 0U : 1U);
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added = mesh_radio_settings_enqueue(settings, MESH_ADMIN_GET_OWNER, 0U);
    added += mesh_radio_settings_enqueue(settings, kind, node_id);
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

int mesh_radio_settings_queue_action(struct mesh_radio_settings *settings,
                                     enum mesh_admin_request_kind kind, uint32_t seconds) {
    if (settings == NULL || !mesh_admin_request_is_action(kind)) {
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
        (mesh_radio_settings_queued(settings, MESH_ADMIN_GET_OWNER, 0U) ? 0U : 1U) +
        (mesh_radio_settings_queued(settings, kind, type) ? 0U : 1U);
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added = mesh_radio_settings_enqueue(settings, MESH_ADMIN_GET_OWNER, 0U);
    added += mesh_radio_settings_enqueue(settings, kind, type);
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
        (mesh_radio_settings_queued(settings, MESH_ADMIN_GET_OWNER, 0U) ? 0U : 1U) +
        (mesh_radio_settings_queued(settings, MESH_ADMIN_SET_TIME, epoch) ? 0U : 1U);
    if (settings->queue_len + needed > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    size_t added = mesh_radio_settings_enqueue(settings, MESH_ADMIN_GET_OWNER, 0U);
    added += mesh_radio_settings_enqueue(settings, MESH_ADMIN_SET_TIME, epoch);
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
    added += mesh_radio_settings_enqueue(settings, MESH_ADMIN_GET_METADATA, 0U);
    added += mesh_radio_settings_enqueue(settings, MESH_ADMIN_GET_OWNER, 0U);
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
        added += mesh_radio_settings_enqueue(settings, MESH_ADMIN_GET_CONFIG, k_config_types[i]);
    }
    /* One per module, from the table rather than a list kept beside it: a module whose row
       exists is refreshed, and one that is not kept is not asked for. */
    for (size_t i = 0; i < mesh_radio_module_count(); ++i) {
        added += mesh_radio_settings_enqueue(settings, MESH_ADMIN_GET_MODULE_CONFIG,
                                             mesh_radio_module_at(i)->admin_type);
    }
    for (uint32_t slot = 0; slot < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++slot) {
        added += mesh_radio_settings_enqueue(settings, MESH_ADMIN_GET_CHANNEL, slot);
    }
    return added;
}

bool mesh_radio_settings_busy(const struct mesh_radio_settings *settings) {
    return settings != NULL && settings->pending_request_id != 0U;
}

bool mesh_radio_settings_next_request(struct mesh_radio_settings *settings, uint64_t now_ms,
                                      struct mesh_admin_request *out) {
    if (settings == NULL || out == NULL) {
        return false;
    }
    if (settings->pending_request_id != 0U) {
        if (now_ms - settings->pending_sent_at_ms < MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS) {
            return false;
        }
        mesh_log_warn("admin", "No reply to admin request %u after %u ms; moving on",
                      settings->pending_request_id, MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS);
        settings->timeouts += 1U;
        if (settings->pending_is_write) {
            mesh_radio_settings_record_write_result(settings, MESH_RADIO_SETTINGS_WRITE_TIMEOUT);
        }
        settings->pending_request_id = 0U;
        settings->pending_sent_at_ms = 0U;
        settings->pending_is_write = false;
    }
    if (settings->queue_len == 0U) {
        return false;
    }
    *out = settings->queue[settings->queue_head];
    settings->queue_head = (settings->queue_head + 1U) % MESH_RADIO_SETTINGS_FETCH_MAX;
    settings->queue_len -= 1U;
    /* Remember what kind went out so the reply (or its absence) is accounted correctly. The
       caller's mark_sent() confirms it actually left. */
    settings->pending_is_write = mesh_admin_request_is_write(out->kind);
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
