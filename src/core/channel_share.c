#define _POSIX_C_SOURCE 200809L

/* See mesh/core/channel_share.h. The half of channel sharing that knows what a radio is. */

#include "mesh/core/channel_share.h"

#include "mesh/proto/channel_url.h"

#include <errno.h>
#include <string.h>

/*
 * Whether a slot already holds the channel a link names.
 *
 * Three fields rather than the whole struct: a name, a key and a role are what make a channel
 * *that channel*, and they are what a reader on the other side of the mesh has to match. A
 * memcmp of two ChannelSettings would also compare the padding a compiler chose and the
 * position-precision the sender happened to have set, so an import from a radio with a slightly
 * different idea about either would rewrite every slot and reboot the radio for nothing.
 */
static bool channel_matches(const meshtastic_Channel *have, const meshtastic_ChannelSettings *want,
                            meshtastic_Channel_Role role) {
    if (!have->has_settings || have->role != role) {
        return false;
    }
    if (strncmp(have->settings.name, want->name, sizeof have->settings.name) != 0) {
        return false;
    }
    return have->settings.psk.size == want->psk.size &&
           memcmp(have->settings.psk.bytes, want->psk.bytes, want->psk.size) == 0;
}

/* True when the slot is holding nothing: never sent, or sent as disabled. */
static bool slot_is_free(const struct mesh_radio_settings *settings, size_t slot) {
    return !settings->has_channel[slot] ||
           settings->channels[slot].role == meshtastic_Channel_Role_DISABLED;
}

size_t mesh_channel_share_build(const struct mesh_radio_settings *settings,
                                meshtastic_ChannelSet *out) {
    if (settings == NULL || out == NULL) {
        return 0U;
    }
    *out = (meshtastic_ChannelSet)meshtastic_ChannelSet_init_zero;

    /*
     * The primary first, and there is nothing to share without one: a ChannelSet whose first
     * entry is a secondary is a set every reader would join under the wrong role, and the radio
     * has told us nothing yet if slot 0 has not arrived.
     */
    if (!settings->has_channel[0] || !settings->channels[0].has_settings ||
        settings->channels[0].role != meshtastic_Channel_Role_PRIMARY) {
        return 0U;
    }
    out->settings[out->settings_count++] = settings->channels[0].settings;

    const pb_size_t room = (pb_size_t)(sizeof out->settings / sizeof out->settings[0]);
    for (size_t i = 1U; i < MESH_RADIO_SETTINGS_MAX_CHANNELS && out->settings_count < room; ++i) {
        if (settings->has_channel[i] && settings->channels[i].has_settings &&
            settings->channels[i].role == meshtastic_Channel_Role_SECONDARY) {
            out->settings[out->settings_count++] = settings->channels[i].settings;
        }
    }

    /* Without the LoRa config a reader joins the right channels on the wrong radio settings and
       hears nothing, which looks exactly like a mistyped key. */
    if (settings->has_lora) {
        out->has_lora_config = true;
        out->lora_config = settings->lora;
    }
    return out->settings_count;
}

size_t mesh_channel_share_url(const struct mesh_radio_settings *settings, char *out,
                              size_t out_len) {
    meshtastic_ChannelSet set;
    if (mesh_channel_share_build(settings, &set) == 0U) {
        if (out != NULL && out_len > 0U) {
            out[0] = '\0';
        }
        return 0U;
    }
    return mesh_channel_url_encode(&set, false, out, out_len);
}

/*
 * One pass over the import, shared by the plan and the queue.
 *
 * Written once and read twice rather than twice and hoped to agree, because the two answers
 * have to be the same answer: a sheet that says "two channels" in front of a write that moves
 * three is worse than no sheet. `queue` NULL is the dry run.
 */
static int import_walk(const struct mesh_radio_settings *settings,
                       const meshtastic_ChannelSet *set, bool add,
                       struct mesh_channel_import_plan *plan, struct mesh_radio_settings *queue) {
    struct mesh_admin_request writes[MESH_RADIO_SETTINGS_MAX_CHANNELS + 1U];
    size_t count = 0U;

    memset(plan, 0, sizeof *plan);
    plan->channels = set->settings_count;

    if (add) {
        /*
         * Keep what this radio has and put the incoming channels in whatever is free, as
         * secondaries - including the sender's primary, which is *their* primary and not this
         * radio's. A channel already held is skipped rather than moved: the same channel in two
         * slots is two conversations to the message log.
         */
        for (size_t i = 0; i < set->settings_count; ++i) {
            bool held = false;
            for (size_t slot = 0; slot < MESH_RADIO_SETTINGS_MAX_CHANNELS && !held; ++slot) {
                held = settings->has_channel[slot] &&
                       (channel_matches(&settings->channels[slot], &set->settings[i],
                                        meshtastic_Channel_Role_SECONDARY) ||
                        channel_matches(&settings->channels[slot], &set->settings[i],
                                        meshtastic_Channel_Role_PRIMARY));
            }
            if (held) {
                continue;
            }
            size_t target = MESH_RADIO_SETTINGS_MAX_CHANNELS;
            for (size_t slot = 1U; slot < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++slot) {
                bool taken = false;
                for (size_t j = 0; j < count && !taken; ++j) {
                    taken = writes[j].type == (uint32_t)slot;
                }
                if (!taken && slot_is_free(settings, slot)) {
                    target = slot;
                    break;
                }
            }
            if (target == MESH_RADIO_SETTINGS_MAX_CHANNELS) {
                plan->full = true; /* the table is full; the rest of the link has nowhere to go */
                break;
            }
            struct mesh_admin_request *write = &writes[count++];
            memset(write, 0, sizeof *write);
            write->kind = MESH_ADMIN_SET_CHANNEL;
            write->type = (uint32_t)target;
            write->payload.channel.index = (int8_t)target;
            write->payload.channel.role = meshtastic_Channel_Role_SECONDARY;
            write->payload.channel.has_settings = true;
            write->payload.channel.settings = set->settings[i];
        }
    } else {
        /* The set replaces the table: its first channel is the primary, the rest follow in
           order, and any slot past them that is in use is switched off. */
        for (size_t slot = 0; slot < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++slot) {
            meshtastic_Channel_Role role = meshtastic_Channel_Role_DISABLED;
            const meshtastic_ChannelSettings *want = NULL;
            if (slot < set->settings_count) {
                role = slot == 0U ? meshtastic_Channel_Role_PRIMARY
                                  : meshtastic_Channel_Role_SECONDARY;
                want = &set->settings[slot];
            } else if (slot_is_free(settings, slot)) {
                continue; /* already off */
            }

            if (want != NULL && settings->has_channel[slot] &&
                channel_matches(&settings->channels[slot], want, role)) {
                continue; /* this radio is already on that channel, in that role */
            }

            struct mesh_admin_request *write = &writes[count++];
            memset(write, 0, sizeof *write);
            write->kind = MESH_ADMIN_SET_CHANNEL;
            write->type = (uint32_t)slot;
            write->payload.channel.index = (int8_t)slot;
            write->payload.channel.role = role;
            write->payload.channel.has_settings = true;
            if (want != NULL) {
                write->payload.channel.settings = *want;
                if (slot == 0U) {
                    plan->replaces_primary = true;
                }
            }
        }
        if (set->settings_count > MESH_RADIO_SETTINGS_MAX_CHANNELS) {
            plan->full = true;
        }
    }

    plan->writes = count;

    /*
     * The LoRa config, and only when the set is replacing the table. An add is somebody handing
     * over one more channel on a mesh this radio is already on; taking their region and modem
     * preset with it would move the radio off every channel it already had.
     */
    if (!add && set->has_lora_config) {
        plan->writes_lora = true;
        struct mesh_admin_request *write = &writes[count++];
        memset(write, 0, sizeof *write);
        write->kind = MESH_ADMIN_SET_CONFIG;
        write->type = (uint32_t)meshtastic_AdminMessage_ConfigType_LORA_CONFIG;
        write->payload.config.which_payload_variant = meshtastic_Config_lora_tag;
        write->payload.config.payload_variant.lora = set->lora_config;
    }

    if (queue == NULL || count == 0U) {
        return 0;
    }

    /*
     * Capacity first, then the writes. Each one costs a set and its read-back, and they share
     * the one passkey refresh in front of them; an over-estimate by that one request is the
     * right way to be wrong here, because the thing being prevented is an import that stops
     * half way and leaves the radio on a mesh that does not exist.
     */
    if (queue->queue_len + 1U + count * 2U > MESH_RADIO_SETTINGS_FETCH_MAX) {
        return -ENOSPC;
    }
    int queued = 0;
    for (size_t i = 0; i < count; ++i) {
        const int added = mesh_radio_settings_queue_write(queue, &writes[i]);
        if (added < 0) {
            return added;
        }
        queued += added;
    }
    return queued;
}

bool mesh_channel_import_plan(const struct mesh_radio_settings *settings,
                              const meshtastic_ChannelSet *set, bool add,
                              struct mesh_channel_import_plan *out) {
    if (settings == NULL || set == NULL || out == NULL || set->settings_count == 0U) {
        return false;
    }
    if (!mesh_radio_settings_loaded(settings)) {
        return false;
    }
    (void)import_walk(settings, set, add, out, NULL);
    return true;
}

int mesh_channel_share_queue_import(struct mesh_radio_settings *settings,
                                    const meshtastic_ChannelSet *set, bool add) {
    if (settings == NULL || set == NULL || set->settings_count == 0U) {
        return -EINVAL;
    }
    if (!mesh_radio_settings_loaded(settings)) {
        return -EINVAL;
    }
    struct mesh_channel_import_plan plan;
    return import_walk(settings, set, add, &plan, settings);
}
