/*
 * A Meshtastic profile to and from a DeviceProfile. See include/mesh/core/radio_profile_cfg.h.
 */

#include "mesh/core/radio_profile_cfg.h"

#include "inkwell/base/file.h"
#include "inkwell/base/text.h"
#include "mesh/core/radio_backup_meshtastic.h"
#include "mesh/core/radio_profile.h"
#include "mesh/proto/channel_url.h"
#include "meshtastic/clientonly.pb.h"

#include <pb_common.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- matching a section to a member ------------------------------------------------------- */

/*
 * The oneof member a Config or ModuleConfig has set, or with `desc` given, the one of that type:
 * where it sits and what it is. LocalConfig and LocalModuleConfig hold the same sub-messages as
 * plain members, so a sub-message's descriptor is what pairs a section with its member.
 */
static bool cfg_oneof(const pb_msgdesc_t *fields, void *message, const pb_msgdesc_t *desc,
                      pb_field_iter_t *out) {
    if (!pb_field_iter_begin(out, fields, message)) {
        return false;
    }
    do {
        if (PB_HTYPE(out->type) != PB_HTYPE_ONEOF || !PB_LTYPE_IS_SUBMSG(out->type)) {
            continue;
        }
        if (desc != NULL ? out->submsg_desc == desc
                         : out->pSize != NULL && *(const pb_size_t *)out->pSize == out->tag) {
            return true;
        }
    } while (pb_field_iter_next(out));
    return false;
}

/* The plain member of a LocalConfig or LocalModuleConfig of type `desc`. */
static bool cfg_member(const pb_msgdesc_t *fields, void *local, const pb_msgdesc_t *desc,
                       pb_field_iter_t *out) {
    if (!pb_field_iter_begin(out, fields, local)) {
        return false;
    }
    do {
        if (PB_LTYPE_IS_SUBMSG(out->type) && out->submsg_desc == desc &&
            PB_HTYPE(out->type) == PB_HTYPE_OPTIONAL && out->pSize != NULL) {
            return true;
        }
    } while (pb_field_iter_next(out));
    return false;
}

/* A section's message into its member of `local`; false for one `local` has no member for. */
static bool cfg_to_local(const pb_msgdesc_t *section_fields, void *section,
                         const pb_msgdesc_t *local_fields, void *local) {
    pb_field_iter_t from;
    pb_field_iter_t to;
    if (!cfg_oneof(section_fields, section, NULL, &from) ||
        !cfg_member(local_fields, local, from.submsg_desc, &to) || to.data_size != from.data_size) {
        return false;
    }
    memcpy(to.pData, from.pData, from.data_size);
    *(bool *)to.pSize = true;
    return true;
}

/* ---- out ----------------------------------------------------------------------------------- */

struct cfg_text {
    const char *text;
};

static bool cfg_encode_text(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg) {
    const struct cfg_text *text = *arg;
    return pb_encode_tag_for_field(stream, field) &&
           pb_encode_string(stream, (const pb_byte_t *)text->text, strlen(text->text));
}

static bool cfg_decode_section(const uint8_t *data, size_t len, const pb_msgdesc_t *fields,
                               void *out) {
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    return pb_decode(&stream, fields, out);
}

/* A text section - canned messages, a ringtone - into a fixed field, when it fits whole. */
static bool cfg_text_section(const uint8_t *data, size_t len, char *out, size_t out_len) {
    if (len == 0U || len > out_len || data[len - 1U] != '\0') {
        return false;
    }
    memcpy(out, data, len);
    return true;
}

/* The channel table as a link: the primary first, then every secondary, then the LoRa config. */
static void cfg_channel_set(const meshtastic_Channel *channels, const bool *has,
                            const meshtastic_Config_LoRaConfig *lora, meshtastic_ChannelSet *set) {
    *set = (meshtastic_ChannelSet)meshtastic_ChannelSet_init_zero;
    const size_t room = sizeof set->settings / sizeof set->settings[0];
    for (int pass = 0; pass < 2; ++pass) {
        const meshtastic_Channel_Role role =
            pass == 0 ? meshtastic_Channel_Role_PRIMARY : meshtastic_Channel_Role_SECONDARY;
        for (size_t i = 0; i < MESH_RADIO_BACKUP_CHANNELS && set->settings_count < room; ++i) {
            if (has[i] && channels[i].role == role && channels[i].has_settings) {
                set->settings[set->settings_count++] = channels[i].settings;
            }
        }
    }
    if (lora != NULL) {
        set->has_lora_config = true;
        set->lora_config = *lora;
    }
}

int mesh_radio_profile_cfg_encode(const struct mesh_radio_backup *profile, uint8_t *out,
                                  size_t out_len) {
    if (profile == NULL || out == NULL || profile->header.reason != MESH_RADIO_BACKUP_PROFILE ||
        profile->header.protocol != MESH_RADIO_BACKUP_MESHTASTIC) {
        return -EINVAL;
    }
    meshtastic_DeviceProfile *device = calloc(1U, sizeof *device);
    meshtastic_Channel *channels = calloc(MESH_RADIO_BACKUP_CHANNELS, sizeof *channels);
    char *url = malloc(MESH_CHANNEL_URL_MAX);
    if (device == NULL || channels == NULL || url == NULL) {
        free(device);
        free(channels);
        free(url);
        return -ENOMEM;
    }
    bool has_channel[MESH_RADIO_BACKUP_CHANNELS] = {false};
    int result = 0;
    const uint8_t *data = NULL;
    const struct mesh_radio_backup_section *section = NULL;
    for (size_t i = 0;
         result == 0 && (section = mesh_radio_backup_section_at(profile, i, &data)) != NULL; ++i) {
        switch (section->tag) {
        case MESH_RADIO_BACKUP_MT_CONFIG: {
            meshtastic_Config config = meshtastic_Config_init_zero;
            if (!cfg_decode_section(data, section->len, meshtastic_Config_fields, &config)) {
                result = -EBADMSG;
            } else if (cfg_to_local(meshtastic_Config_fields, &config,
                                    meshtastic_LocalConfig_fields, &device->config)) {
                device->has_config = true;
            }
            break;
        }
        case MESH_RADIO_BACKUP_MT_MODULE: {
            meshtastic_ModuleConfig module = meshtastic_ModuleConfig_init_zero;
            if (!cfg_decode_section(data, section->len, meshtastic_ModuleConfig_fields, &module)) {
                result = -EBADMSG;
            } else if (cfg_to_local(meshtastic_ModuleConfig_fields, &module,
                                    meshtastic_LocalModuleConfig_fields, &device->module_config)) {
                device->has_module_config = true;
            }
            break;
        }
        case MESH_RADIO_BACKUP_MT_CHANNEL: {
            meshtastic_Channel channel = meshtastic_Channel_init_zero;
            if (!cfg_decode_section(data, section->len, meshtastic_Channel_fields, &channel)) {
                result = -EBADMSG;
            } else if (channel.index >= 0 && channel.index < (int8_t)MESH_RADIO_BACKUP_CHANNELS) {
                channels[channel.index] = channel;
                has_channel[channel.index] = true;
            }
            break;
        }
        case MESH_RADIO_BACKUP_MT_CANNED:
            device->has_canned_messages = cfg_text_section(
                data, section->len, device->canned_messages, sizeof device->canned_messages);
            break;
        case MESH_RADIO_BACKUP_MT_RINGTONE:
            device->has_ringtone =
                cfg_text_section(data, section->len, device->ringtone, sizeof device->ringtone);
            break;
        default:
            break; /* the UI config has no place in a DeviceProfile */
        }
    }
    struct cfg_text text = {.text = url};
    if (result == 0 &&
        mesh_radio_profile_parts_has(&profile->header.parts, MESH_RADIO_BACKUP_TOPIC_CHANNEL, 0U)) {
        meshtastic_ChannelSet set;
        cfg_channel_set(channels, has_channel,
                        device->config.has_lora ? &device->config.lora : NULL, &set);
        if (set.settings_count > 0U &&
            mesh_channel_url_encode(&set, false, url, MESH_CHANNEL_URL_MAX) > 0U) {
            device->channel_url.funcs.encode = cfg_encode_text;
            device->channel_url.arg = &text;
        }
    }
    size_t written = 0U;
    if (result == 0) {
        pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
        if (pb_encode(&stream, meshtastic_DeviceProfile_fields, device)) {
            written = stream.bytes_written;
        } else {
            result = -ENOSPC;
        }
    }
    free(device);
    free(channels);
    free(url);
    return result == 0 ? (int)written : result;
}

/* ---- in ------------------------------------------------------------------------------------ */

struct cfg_url {
    char *text;
    size_t max;
    bool seen;
    bool fits;
};

static bool cfg_decode_url(pb_istream_t *stream, const pb_field_iter_t *field, void **arg) {
    (void)field;
    struct cfg_url *url = *arg;
    const size_t len = stream->bytes_left;
    url->seen = true;
    url->fits = len < url->max;
    if (!url->fits) {
        return pb_read(stream, NULL, len);
    }
    if (!pb_read(stream, (pb_byte_t *)url->text, len)) {
        return false;
    }
    url->text[len] = '\0';
    return true;
}

/* Every member a LocalConfig or LocalModuleConfig has set, as a section of `tag`. */
static int cfg_from_local(const pb_msgdesc_t *local_fields, void *local,
                          const pb_msgdesc_t *section_fields, size_t section_size, uint16_t tag,
                          struct mesh_radio_backup *out) {
    pb_field_iter_t member;
    if (!pb_field_iter_begin(&member, local_fields, local)) {
        return 0;
    }
    void *section = malloc(section_size);
    if (section == NULL) {
        return -ENOMEM;
    }
    int result = 0;
    do {
        if (!PB_LTYPE_IS_SUBMSG(member.type) || PB_HTYPE(member.type) != PB_HTYPE_OPTIONAL ||
            member.pSize == NULL || !*(const bool *)member.pSize) {
            continue;
        }
        memset(section, 0, section_size);
        pb_field_iter_t variant;
        if (!cfg_oneof(section_fields, section, member.submsg_desc, &variant) ||
            variant.data_size != member.data_size) {
            continue; /* a member this build's Config or ModuleConfig has no variant for */
        }
        *(pb_size_t *)variant.pSize = variant.tag;
        memcpy(variant.pData, member.pData, member.data_size);
        uint8_t bytes[MESH_RADIO_BACKUP_SECTION_MAX];
        pb_ostream_t stream = pb_ostream_from_buffer(bytes, sizeof bytes);
        if (!pb_encode(&stream, section_fields, section)) {
            result = -EBADMSG;
            break;
        }
        result = mesh_radio_backup_add(out, tag, bytes, stream.bytes_written);
    } while (result == 0 && pb_field_iter_next(&member));
    free(section);
    return result;
}

static int cfg_add_channel(struct mesh_radio_backup *out, const meshtastic_Channel *channel) {
    uint8_t bytes[MESH_RADIO_BACKUP_SECTION_MAX];
    pb_ostream_t stream = pb_ostream_from_buffer(bytes, sizeof bytes);
    if (!pb_encode(&stream, meshtastic_Channel_fields, channel)) {
        return -EBADMSG;
    }
    return mesh_radio_backup_add(out, MESH_RADIO_BACKUP_MT_CHANNEL, bytes, stream.bytes_written);
}

/* The link's channels as the table they replace: primary, secondaries, and the rest off. */
static int cfg_add_channels(struct mesh_radio_backup *out, const meshtastic_ChannelSet *set) {
    int result = 0;
    for (size_t slot = 0; result == 0 && slot < MESH_RADIO_BACKUP_CHANNELS; ++slot) {
        meshtastic_Channel channel = meshtastic_Channel_init_zero;
        channel.index = (int8_t)slot;
        /* A slot switched off still carries settings, empty ones: it is how the firmware reports
           one (a Heltec V3 on 2.7.26), and how an import's own write clears one (channel_share.c).
           Absent, every empty slot of the table compared as a difference with the radio. */
        channel.has_settings = true;
        if (slot < set->settings_count) {
            channel.role =
                slot == 0U ? meshtastic_Channel_Role_PRIMARY : meshtastic_Channel_Role_SECONDARY;
            channel.settings = set->settings[slot];
        }
        result = cfg_add_channel(out, &channel);
    }
    return result;
}

static int cfg_add_text(struct mesh_radio_backup *out, uint16_t tag, const char *text) {
    return mesh_radio_backup_add(out, tag, text, strlen(text) + 1U);
}

/* The header's LoRa numbers and channel names, as a capture would have written them. */
static void cfg_header(const meshtastic_DeviceProfile *device, const meshtastic_ChannelSet *set,
                       bool has_set, struct mesh_radio_backup_header *header) {
    const meshtastic_Config_LoRaConfig *lora = device->config.has_lora ? &device->config.lora
                                               : has_set && set->has_lora_config ? &set->lora_config
                                                                                 : NULL;
    if (lora != NULL) {
        inkwell_str_copy(header->region, sizeof header->region,
                         mesh_radio_region_name(lora->region));
        header->has_radio = true;
        header->frequency_khz = lora->override_frequency > 0.0f
                                    ? (uint32_t)(lora->override_frequency * 1000.0f + 0.5f)
                                    : 0U;
        header->tx_power_dbm = (int8_t)lora->tx_power;
        if (lora->use_preset) {
            inkwell_str_copy(header->preset, sizeof header->preset,
                             mesh_radio_modem_preset_name(lora->modem_preset));
        } else {
            header->bandwidth_hz = lora->bandwidth == 31U   ? 31250U
                                   : lora->bandwidth == 62U ? 62500U
                                                            : lora->bandwidth * 1000U;
            header->spreading_factor = (uint8_t)lora->spread_factor;
            header->coding_rate = (uint8_t)lora->coding_rate;
        }
    }
    for (size_t i = 0; has_set && i < set->settings_count && i < MESH_RADIO_BACKUP_CHANNELS; ++i) {
        inkwell_str_copy(header->channel_names[i], sizeof header->channel_names[i],
                         set->settings[i].name);
        header->channel_count = (uint8_t)(i + 1U);
    }
}

int mesh_radio_profile_cfg_decode(const uint8_t *data, size_t len, const char *name,
                                  struct mesh_radio_backup *out) {
    if (data == NULL || name == NULL || name[0] == '\0' || out == NULL) {
        return -EINVAL;
    }
    mesh_radio_backup_reset(out);
    meshtastic_DeviceProfile *device = calloc(1U, sizeof *device);
    meshtastic_ChannelSet *set = calloc(1U, sizeof *set);
    struct mesh_radio_backup *whole = malloc(sizeof *whole);
    char *url_text = malloc(MESH_CHANNEL_URL_MAX);
    if (device == NULL || set == NULL || whole == NULL || url_text == NULL) {
        free(device);
        free(set);
        free(whole);
        free(url_text);
        return -ENOMEM;
    }
    struct cfg_url url = {.text = url_text, .max = MESH_CHANNEL_URL_MAX};
    device->channel_url.funcs.decode = cfg_decode_url;
    device->channel_url.arg = &url;
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    int result = pb_decode(&stream, meshtastic_DeviceProfile_fields, device) ? 0 : -EBADMSG;
    bool has_set = false;
    if (result == 0 && url.seen) {
        bool add = false;
        has_set = url.fits && mesh_channel_url_decode(url.text, set, &add);
        if (!has_set) {
            result = -EBADMSG; /* a link that does not parse is a table we cannot say anything of */
        }
    }

    /* Everything the file has, as a backup would hold it; then made into a profile, which is
       what takes the identity back out - the Security section - and says which parts are in. */
    mesh_radio_backup_reset(whole);
    whole->header.protocol = MESH_RADIO_BACKUP_MESHTASTIC;
    if (result == 0 && device->has_config) {
        /* A LoRa config that came only with the link is the link's: see below. */
        result =
            cfg_from_local(meshtastic_LocalConfig_fields, &device->config, meshtastic_Config_fields,
                           sizeof(meshtastic_Config), MESH_RADIO_BACKUP_MT_CONFIG, whole);
    }
    if (result == 0 && !device->config.has_lora && has_set && set->has_lora_config) {
        meshtastic_LocalConfig lora = meshtastic_LocalConfig_init_zero;
        lora.has_lora = true;
        lora.lora = set->lora_config;
        result = cfg_from_local(meshtastic_LocalConfig_fields, &lora, meshtastic_Config_fields,
                                sizeof(meshtastic_Config), MESH_RADIO_BACKUP_MT_CONFIG, whole);
    }
    if (result == 0 && device->has_module_config) {
        result = cfg_from_local(meshtastic_LocalModuleConfig_fields, &device->module_config,
                                meshtastic_ModuleConfig_fields, sizeof(meshtastic_ModuleConfig),
                                MESH_RADIO_BACKUP_MT_MODULE, whole);
    }
    if (result == 0 && has_set) {
        result = cfg_add_channels(whole, set);
    }
    if (result == 0 && device->has_canned_messages) {
        result = cfg_add_text(whole, MESH_RADIO_BACKUP_MT_CANNED, device->canned_messages);
    }
    if (result == 0 && device->has_ringtone) {
        result = cfg_add_text(whole, MESH_RADIO_BACKUP_MT_RINGTONE, device->ringtone);
    }
    if (result == 0) {
        cfg_header(device, set, has_set, &whole->header);
        const struct mesh_radio_backup_parts every = {.topics = UINT32_MAX, .modules = UINT32_MAX};
        result = mesh_radio_profile_make(whole, &every, name, out);
    }
    free(device);
    free(set);
    free(whole);
    free(url_text);
    if (result != 0) {
        mesh_radio_backup_reset(out);
    }
    return result;
}

/* ---- files --------------------------------------------------------------------------------- */

int mesh_radio_profile_cfg_write(const struct mesh_radio_backup *profile, const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -EINVAL;
    }
    uint8_t *bytes = malloc(MESH_RADIO_PROFILE_CFG_MAX);
    if (bytes == NULL) {
        return -ENOMEM;
    }
    const int len = mesh_radio_profile_cfg_encode(profile, bytes, MESH_RADIO_PROFILE_CFG_MAX);
    int result = len < 0 ? len : 0;
    char temp[MESH_RADIO_BACKUP_PATH_MAX + 80U];
    if (result == 0 && snprintf(temp, sizeof temp, "%s.tmp", path) >= (int)sizeof temp) {
        result = -ENAMETOOLONG;
    }
    if (result == 0) {
        FILE *file = fopen(temp, "wb");
        if (file == NULL) {
            result = -errno;
        } else {
            const bool whole = fwrite(bytes, 1U, (size_t)len, file) == (size_t)len;
            const bool closed = fclose(file) == 0;
            result = whole && closed ? inkwell_file_replace(temp, path) : -EIO;
            if (result != 0) {
                (void)remove(temp);
            }
        }
    }
    free(bytes);
    return result;
}

int mesh_radio_profile_cfg_read(const char *path, const char *name, struct mesh_radio_backup *out) {
    if (path == NULL || out == NULL) {
        return -EINVAL;
    }
    size_t len = 0U;
    uint8_t *bytes = inkwell_file_read(path, MESH_RADIO_PROFILE_CFG_MAX, &len);
    if (bytes == NULL) {
        return -ENOENT;
    }
    const int result = mesh_radio_profile_cfg_decode(bytes, len, name, out);
    free(bytes);
    return result;
}
