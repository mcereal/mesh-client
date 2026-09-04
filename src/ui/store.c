#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/store.h"

#include "mesh/log.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static void mesh_ui_store_mark_dirty(struct mesh_ui_store *store, mesh_ui_update_flags flags) {
    if (store == NULL || flags == MESH_UI_UPDATE_NONE) {
        return;
    }

    store->pending_flags |= flags;

    if (store->event_fd >= 0) {
        const uint64_t value = 1U;
        if (write(store->event_fd, &value, sizeof value) < 0) {
            if (errno != EAGAIN) {
                mesh_log_warn("ui", "eventfd write failed: %s", strerror(errno));
            }
        }
    }
}

int mesh_ui_store_init(struct mesh_ui_store *store) {
    if (store == NULL) {
        return -EINVAL;
    }

    memset(store, 0, sizeof *store);
    mesh_ui_nav_init(&store->nav);
    store->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (store->event_fd < 0) {
        const int err = -errno;
        mesh_log_error("ui", "eventfd create failed: %s", strerror(errno));
        store->event_fd = -1;
        return err;
    }

    return 0;
}

void mesh_ui_store_shutdown(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }

    if (store->event_fd >= 0) {
        close(store->event_fd);
        store->event_fd = -1;
    }
    store->pending_flags = MESH_UI_UPDATE_NONE;
    store->device_count = 0U;
    store->handshake_valid = false;
    memset(&store->messages, 0, sizeof store->messages);
}

void mesh_ui_store_reset(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_store preserved = *store;
    memset(store, 0, sizeof *store);
    mesh_ui_nav_init(&store->nav);
    store->event_fd = preserved.event_fd;
    store->pending_flags = preserved.pending_flags;
}

bool mesh_ui_store_handle_key(struct mesh_ui_store *store, enum mesh_ui_key key,
                              struct mesh_ui_action *out_action) {
    if (store == NULL) {
        if (out_action != NULL) {
            memset(out_action, 0, sizeof *out_action);
        }
        return false;
    }

    /* Lists may have changed since the last frame; a stale cursor would act on the wrong row. */
    mesh_ui_nav_clamp(&store->nav, store);
    const bool changed = mesh_ui_nav_handle_key(&store->nav, store, key, out_action);
    if (changed) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
    }
    return changed;
}

void mesh_ui_store_set_toast(struct mesh_ui_store *store, uint64_t now_ms, const char *text) {
    if (store == NULL) {
        return;
    }
    mesh_ui_nav_set_toast(&store->nav, now_ms, text);
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
}

void mesh_ui_store_settings_edits_clear(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }
    if (store->nav.settings_edit_count == 0U && !store->nav.settings_discard_armed &&
        !store->nav.confirm_open) {
        return;
    }
    memset(store->nav.settings_edits, 0, sizeof store->nav.settings_edits);
    store->nav.settings_edit_count = 0U;
    store->nav.settings_discard_armed = false;
    store->nav.confirm_open = false;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
}

void mesh_ui_store_tick(struct mesh_ui_store *store, uint64_t now_ms) {
    if (store == NULL) {
        return;
    }
    if (mesh_ui_nav_tick(&store->nav, now_ms)) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
    }
}

int mesh_ui_store_event_fd(const struct mesh_ui_store *store) {
    if (store == NULL) {
        return -1;
    }
    return store->event_fd;
}

void mesh_ui_store_set_discovery(struct mesh_ui_store *store, const struct mesh_ui_device *devices,
                                 size_t count) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_device next[MESH_UI_MAX_DEVICES];
    memset(next, 0, sizeof(next));

    const size_t capped = (count > MESH_UI_MAX_DEVICES) ? MESH_UI_MAX_DEVICES : count;
    if (devices != NULL && capped > 0U) {
        memcpy(next, devices, capped * sizeof(struct mesh_ui_device));
    }

    const bool count_changed = (store->device_count != capped);
    const bool payload_changed = (memcmp(store->devices, next, sizeof(next)) != 0);

    if (!count_changed && !payload_changed) {
        return;
    }

    memcpy(store->devices, next, sizeof(next));
    store->device_count = capped;

    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_DISCOVERY);
}

void mesh_ui_store_set_handshake(struct mesh_ui_store *store,
                                 const struct mesh_ui_handshake_state *handshake) {
    if (store == NULL) {
        return;
    }

    bool next_valid = (handshake != NULL);
    struct mesh_ui_handshake_state next_state;
    memset(&next_state, 0, sizeof(next_state));
    if (next_valid) {
        next_state = *handshake;
    }

    const bool validity_changed = (store->handshake_valid != next_valid);
    const bool payload_changed =
        next_valid && (memcmp(&store->handshake, &next_state, sizeof(next_state)) != 0);

    if (!validity_changed && !payload_changed) {
        return;
    }

    if (next_valid) {
        store->handshake = next_state;
    } else {
        memset(&store->handshake, 0, sizeof store->handshake);
    }
    store->handshake_valid = next_valid;

    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_HANDSHAKE);
}

void mesh_ui_store_set_transport_status(struct mesh_ui_store *store, const char *status) {
    if (store == NULL) {
        return;
    }

    char next[MESH_UI_TRANSPORT_STATUS_MAX];
    memset(next, 0, sizeof next);
    if (status != NULL) {
        snprintf(next, sizeof next, "%s", status);
    }

    if (memcmp(store->transport_status, next, sizeof next) == 0) {
        return;
    }

    memcpy(store->transport_status, next, sizeof store->transport_status);
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_TRANSPORT);
}

void mesh_ui_store_set_settings(struct mesh_ui_store *store,
                                const struct mesh_ui_settings *settings) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_settings next;
    memset(&next, 0, sizeof next);
    if (settings != NULL) {
        next = *settings;
    }
    if (memcmp(&store->settings, &next, sizeof next) == 0) {
        return;
    }
    store->settings = next;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_SETTINGS);
}

void mesh_ui_store_set_messages(struct mesh_ui_store *store,
                                const struct mesh_ui_message_list *messages) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_message_list next;
    memset(&next, 0, sizeof(next));
    if (messages != NULL) {
        next = *messages;
        if (next.count > MESH_UI_MAX_MESSAGES) {
            next.count = MESH_UI_MAX_MESSAGES;
        }
        /* Zero the unused tail so the memcmp below compares like with like. */
        for (uint32_t i = next.count; i < MESH_UI_MAX_MESSAGES; ++i) {
            memset(&next.entries[i], 0, sizeof(next.entries[i]));
        }
    }

    if (memcmp(&store->messages, &next, sizeof(next)) == 0) {
        return;
    }

    store->messages = next;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_MESSAGES);
}

static bool mesh_ui_message_list_contains(const struct mesh_ui_message_list *list,
                                          uint32_t packet_id) {
    /* Packet id 0 means "no id" in the Meshtastic protocol, so it never identifies anything. */
    if (list == NULL || packet_id == 0U) {
        return false;
    }
    for (uint32_t i = 0; i < list->count && i < MESH_UI_MAX_MESSAGES; ++i) {
        if (list->entries[i].packet_id == packet_id) {
            return true;
        }
    }
    return false;
}

void mesh_ui_message_list_merge(const struct mesh_ui_message_list *cached,
                                const struct mesh_ui_message_list *live,
                                struct mesh_ui_message_list *out) {
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    const uint32_t live_count =
        (live == NULL) ? 0U
                       : (live->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : live->count);

    /* Live messages are the newest and always keep their slots; history fills what is left. */
    const uint32_t room = MESH_UI_MAX_MESSAGES - live_count;

    uint32_t eligible[MESH_UI_MAX_MESSAGES];
    uint32_t eligible_count = 0U;
    if (cached != NULL) {
        for (uint32_t i = 0; i < cached->count && i < MESH_UI_MAX_MESSAGES; ++i) {
            if (!mesh_ui_message_list_contains(live, cached->entries[i].packet_id)) {
                eligible[eligible_count++] = i;
            }
        }
    }

    /* Too much history for the room left: drop the oldest of it. */
    const uint32_t skipped = (eligible_count > room) ? eligible_count - room : 0U;

    for (uint32_t i = skipped; i < eligible_count; ++i) {
        out->entries[out->count++] = cached->entries[eligible[i]];
    }
    for (uint32_t i = 0; i < live_count; ++i) {
        out->entries[out->count++] = live->entries[i];
    }

    out->dropped =
        ((cached != NULL) ? cached->dropped : 0U) + ((live != NULL) ? live->dropped : 0U) + skipped;
}

void mesh_ui_store_request_refresh(struct mesh_ui_store *store) {
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_DISCOVERY | MESH_UI_UPDATE_HANDSHAKE |
                                        MESH_UI_UPDATE_TRANSPORT | MESH_UI_UPDATE_MESSAGES |
                                        MESH_UI_UPDATE_NAV);
}

bool mesh_ui_store_consume_updates(struct mesh_ui_store *store, struct mesh_ui_snapshot *snapshot) {
    if (store == NULL || snapshot == NULL) {
        return false;
    }

    if (store->event_fd >= 0) {
        uint64_t value = 0;
        ssize_t read_result = read(store->event_fd, &value, sizeof value);
        if (read_result < 0 && errno != EAGAIN) {
            mesh_log_warn("ui", "eventfd read failed: %s", strerror(errno));
        }
    }

    if (store->pending_flags == MESH_UI_UPDATE_NONE) {
        return false;
    }

    snapshot->update_flags = store->pending_flags;
    snapshot->device_count = store->device_count;
    if (store->device_count > 0U) {
        memcpy(snapshot->devices, store->devices,
               store->device_count * sizeof(struct mesh_ui_device));
    }
    if (store->device_count < MESH_UI_MAX_DEVICES) {
        memset(&snapshot->devices[store->device_count], 0,
               (MESH_UI_MAX_DEVICES - store->device_count) * sizeof(struct mesh_ui_device));
    }

    snapshot->handshake_valid = store->handshake_valid;
    if (store->handshake_valid) {
        snapshot->handshake = store->handshake;
    } else {
        memset(&snapshot->handshake, 0, sizeof snapshot->handshake);
    }

    snapshot->messages = store->messages;
    snapshot->settings = store->settings;

    memcpy(snapshot->transport_status, store->transport_status, sizeof snapshot->transport_status);

    /* Data changes (a node dropping out, a message arriving) move or invalidate cursors; fix
       them up here so every backend draws a cursor that points at a real row. */
    if (mesh_ui_nav_clamp(&store->nav, store)) {
        store->pending_flags |= MESH_UI_UPDATE_NAV;
        snapshot->update_flags = store->pending_flags;
    }
    snapshot->nav = store->nav;

    store->pending_flags = MESH_UI_UPDATE_NONE;
    return true;
}

static void mesh_ui_store_escape_and_write(FILE *file, const char *key, const char *value) {
    fprintf(file, "%s=", key);
    if (value != NULL) {
        for (const unsigned char *ptr = (const unsigned char *)value; *ptr != '\0'; ++ptr) {
            if (*ptr < 0x20U || *ptr == '\\' || *ptr == '=') {
                fprintf(file, "\\x%02x", *ptr);
            } else {
                fputc(*ptr, file);
            }
        }
    }
    fputc('\n', file);
}

static int mesh_ui_store_save_handshake(FILE *file,
                                        const struct mesh_ui_handshake_state *handshake) {
    if (file == NULL || handshake == NULL) {
        return 0;
    }

    fprintf(file, "handshake_request=%u,%u\n", handshake->request_in_flight ? 1U : 0U,
            handshake->request_id);
    fprintf(file, "handshake_config=%u,%u,%u\n", handshake->config_complete ? 1U : 0U,
            handshake->config_complete_id, handshake->has_config ? 1U : 0U);
    fprintf(file, "handshake_mynode=%u,%u,%u,%u\n", handshake->has_my_info ? 1U : 0U,
            handshake->my_info.node_num, handshake->my_info.nodedb_entries,
            handshake->my_info.reboot_count);
    mesh_ui_store_escape_and_write(file, "handshake_channel", handshake->primary_channel);
    mesh_ui_store_escape_and_write(file, "handshake_my_short", handshake->my_short_name);
    fprintf(file, "handshake_cached=%u\n", handshake->cached ? 1U : 0U);
    fprintf(file, "handshake_channels=%u\n", handshake->channel_count);
    for (uint32_t i = 0; i < handshake->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
        const struct mesh_ui_channel *channel = &handshake->channels[i];
        fprintf(file, "channel[%u]=%u,%u\n", i, (unsigned)channel->index, (unsigned)channel->role);
        char key[32];
        snprintf(key, sizeof key, "channel_name[%u]", i);
        mesh_ui_store_escape_and_write(file, key, channel->name);
    }
    fprintf(file, "handshake_nodes=%u\n", handshake->node_count);
    for (uint32_t i = 0; i < handshake->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
        const struct mesh_ui_node_summary *node = &handshake->nodes[i];
        fprintf(file, "node[%u]=%u,%u,%u,%f,%u,%u\n", i, node->node_id, node->last_heard,
                node->has_hops_away ? 1U : 0U, (double)node->snr, node->via_mqtt ? 1U : 0U,
                node->hops_away);
        char key_long[32];
        char key_short[32];
        snprintf(key_long, sizeof key_long, "node_long[%u]", i);
        snprintf(key_short, sizeof key_short, "node_short[%u]", i);
        mesh_ui_store_escape_and_write(file, key_long, node->long_name);
        mesh_ui_store_escape_and_write(file, key_short, node->short_name);
    }

    return 0;
}

static void mesh_ui_store_unescape_value(char *value) {
    if (value == NULL) {
        return;
    }

    char *write_ptr = value;
    for (char *read_ptr = value; *read_ptr != '\0'; ++read_ptr) {
        if (*read_ptr == '\\') {
            if (read_ptr[1] == 'x' && read_ptr[2] != '\0' && read_ptr[3] != '\0') {
                char hex[3] = {read_ptr[2], read_ptr[3], '\0'};
                *write_ptr++ = (char)strtol(hex, NULL, 16);
                read_ptr += 3;
            }
        } else {
            *write_ptr++ = *read_ptr;
        }
    }
    *write_ptr = '\0';
}

static void mesh_ui_store_save_messages(FILE *file, const struct mesh_ui_message_list *messages) {
    if (file == NULL || messages == NULL) {
        return;
    }

    fprintf(file, "messages=%u,%u\n", messages->count, messages->dropped);
    for (uint32_t i = 0; i < messages->count && i < MESH_UI_MAX_MESSAGES; ++i) {
        const struct mesh_ui_message *message = &messages->entries[i];
        fprintf(file, "msg[%u]=%u,%u,%u,%u,%u,%u,%u\n", i, message->packet_id, message->peer,
                message->rx_time, (unsigned)message->channel, (unsigned)message->direction,
                (unsigned)message->ack, message->broadcast ? 1U : 0U);

        char key_name[32];
        char key_text[32];
        snprintf(key_name, sizeof key_name, "msg_name[%u]", i);
        snprintf(key_text, sizeof key_text, "msg_text[%u]", i);
        mesh_ui_store_escape_and_write(file, key_name, message->peer_name);
        mesh_ui_store_escape_and_write(file, key_text, message->text);
    }
}

int mesh_ui_store_save(const struct mesh_ui_store *store, const char *path) {
    if (store == NULL || path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return -errno;
    }

    fprintf(file, "handshake_valid=%u\n", store->handshake_valid ? 1U : 0U);
    if (store->handshake_valid) {
        mesh_ui_store_save_handshake(file, &store->handshake);
    }
    mesh_ui_store_save_messages(file, &store->messages);

    fclose(file);
    return 0;
}

int mesh_ui_store_load(struct mesh_ui_store *store, const char *path) {
    if (store == NULL || path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -errno;
    }

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof(handshake));
    bool handshake_valid = false;
    uint32_t nodes_expected = 0U;
    bool nodes_expected_set = false;
    uint32_t nodes_loaded = 0U;

    struct mesh_ui_message_list messages;
    memset(&messages, 0, sizeof(messages));
    uint32_t messages_expected = 0U;
    bool messages_expected_set = false;
    uint32_t messages_loaded = 0U;

    char line[1280];
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        char *equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }

        *equals = '\0';
        char *key = line;
        char *value = equals + 1;
        mesh_ui_store_unescape_value(value);

        if (strcmp(key, "handshake_valid") == 0) {
            handshake_valid = (strtoul(value, NULL, 10) != 0U);
        } else if (strcmp(key, "handshake_request") == 0) {
            unsigned int inflight = 0U;
            unsigned int request_id = 0U;
            if (sscanf(value, "%u,%u", &inflight, &request_id) == 2) {
                handshake.request_in_flight = (inflight != 0U);
                handshake.request_id = request_id;
            }
        } else if (strcmp(key, "handshake_config") == 0) {
            unsigned int complete = 0U;
            unsigned int config_id = 0U;
            unsigned int has_config = 0U;
            if (sscanf(value, "%u,%u,%u", &complete, &config_id, &has_config) == 3) {
                handshake.config_complete = (complete != 0U);
                handshake.config_complete_id = config_id;
                handshake.has_config = (has_config != 0U);
            }
        } else if (strcmp(key, "handshake_mynode") == 0) {
            unsigned int has_my_info = 0U;
            unsigned int node_num = 0U;
            unsigned int nodedb = 0U;
            unsigned int reboot_count = 0U;
            if (sscanf(value, "%u,%u,%u,%u", &has_my_info, &node_num, &nodedb, &reboot_count) ==
                4) {
                handshake.has_my_info = (has_my_info != 0U);
                handshake.my_info.node_num = node_num;
                handshake.my_info.nodedb_entries = nodedb;
                handshake.my_info.reboot_count = reboot_count;
            }
        } else if (strcmp(key, "handshake_channel") == 0) {
            snprintf(handshake.primary_channel, sizeof(handshake.primary_channel), "%s", value);
        } else if (strcmp(key, "handshake_my_short") == 0) {
            snprintf(handshake.my_short_name, sizeof(handshake.my_short_name), "%s", value);
        } else if (strcmp(key, "handshake_nodes") == 0) {
            nodes_expected = (uint32_t)strtoul(value, NULL, 10);
            nodes_expected_set = true;
        } else if (strcmp(key, "handshake_cached") == 0) {
            handshake.cached = (strtoul(value, NULL, 10) != 0U);
        } else if (strcmp(key, "handshake_channels") == 0) {
            uint32_t count = (uint32_t)strtoul(value, NULL, 10);
            handshake.channel_count = count > MESH_UI_MAX_CHANNELS ? MESH_UI_MAX_CHANNELS : count;
        } else if (strncmp(key, "channel[", 8) == 0) {
            unsigned int index = 0U;
            unsigned int slot = 0U;
            unsigned int role = 0U;
            if (sscanf(key, "channel[%u]", &index) == 1 && index < MESH_UI_MAX_CHANNELS &&
                sscanf(value, "%u,%u", &slot, &role) == 2) {
                handshake.channels[index].index = (uint8_t)slot;
                handshake.channels[index].role = (uint8_t)role;
            }
        } else if (strncmp(key, "channel_name[", 13) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "channel_name[%u]", &index) == 1 && index < MESH_UI_MAX_CHANNELS) {
                snprintf(handshake.channels[index].name, sizeof(handshake.channels[index].name),
                         "%s", value);
            }
        } else if (strncmp(key, "node[", 5) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "node[%u]", &index) == 1) {
                uint32_t node_id = 0U;
                uint32_t last_heard = 0U;
                unsigned int has_hops = 0U;
                double snr = 0.0;
                unsigned int via_mqtt = 0U;
                unsigned int hops = 0U;
                if (sscanf(value, "%u,%u,%u,%lf,%u,%u", &node_id, &last_heard, &has_hops, &snr,
                           &via_mqtt, &hops) == 6) {
                    if (index < MESH_UI_MAX_HANDSHAKE_NODES) {
                        struct mesh_ui_node_summary *node = &handshake.nodes[index];
                        node->node_id = node_id;
                        node->last_heard = last_heard;
                        node->has_hops_away = (has_hops != 0U);
                        node->snr = (float)snr;
                        node->via_mqtt = (via_mqtt != 0U);
                        node->hops_away = (uint8_t)hops;
                        if ((uint32_t)(index + 1U) > nodes_loaded) {
                            nodes_loaded = index + 1U;
                        }
                    }
                }
            }
        } else if (strncmp(key, "node_long[", 10) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "node_long[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES) {
                snprintf(handshake.nodes[index].long_name, sizeof(handshake.nodes[index].long_name),
                         "%s", value);
            }
        } else if (strncmp(key, "node_short[", 11) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "node_short[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES) {
                snprintf(handshake.nodes[index].short_name,
                         sizeof(handshake.nodes[index].short_name), "%s", value);
            }
        } else if (strcmp(key, "messages") == 0) {
            unsigned int count = 0U;
            unsigned int dropped = 0U;
            if (sscanf(value, "%u,%u", &count, &dropped) == 2) {
                messages_expected = count;
                messages_expected_set = true;
                messages.dropped = dropped;
            }
        } else if (strncmp(key, "msg[", 4) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "msg[%u]", &index) == 1 && index < MESH_UI_MAX_MESSAGES) {
                unsigned int packet_id = 0U;
                unsigned int peer = 0U;
                unsigned int rx_time = 0U;
                unsigned int channel = 0U;
                unsigned int direction = 0U;
                unsigned int ack = 0U;
                unsigned int broadcast = 0U;
                if (sscanf(value, "%u,%u,%u,%u,%u,%u,%u", &packet_id, &peer, &rx_time, &channel,
                           &direction, &ack, &broadcast) == 7) {
                    struct mesh_ui_message *message = &messages.entries[index];
                    message->packet_id = packet_id;
                    message->peer = peer;
                    message->rx_time = rx_time;
                    message->channel = (uint8_t)channel;
                    message->direction = (uint8_t)direction;
                    message->ack = (uint8_t)ack;
                    message->broadcast = (broadcast != 0U);
                    if ((uint32_t)(index + 1U) > messages_loaded) {
                        messages_loaded = index + 1U;
                    }
                }
            }
        } else if (strncmp(key, "msg_name[", 9) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "msg_name[%u]", &index) == 1 && index < MESH_UI_MAX_MESSAGES) {
                snprintf(messages.entries[index].peer_name,
                         sizeof(messages.entries[index].peer_name), "%s", value);
            }
        } else if (strncmp(key, "msg_text[", 9) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "msg_text[%u]", &index) == 1 && index < MESH_UI_MAX_MESSAGES) {
                snprintf(messages.entries[index].text, sizeof(messages.entries[index].text), "%s",
                         value);
            }
        }
    }

    fclose(file);

    uint32_t final_count = nodes_expected_set ? nodes_expected : nodes_loaded;
    if (final_count > MESH_UI_MAX_HANDSHAKE_NODES) {
        final_count = MESH_UI_MAX_HANDSHAKE_NODES;
    }
    handshake.node_count = final_count;

    if (handshake_valid) {
        if (!handshake.cached) {
            handshake.cached = true;
        }
        store->handshake = handshake;
        store->handshake_valid = true;
    } else {
        memset(&store->handshake, 0, sizeof store->handshake);
        store->handshake_valid = false;
    }

    uint32_t message_count = messages_expected_set ? messages_expected : messages_loaded;
    if (message_count > MESH_UI_MAX_MESSAGES) {
        message_count = MESH_UI_MAX_MESSAGES;
    }
    messages.count = message_count;
    store->messages = messages;

    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_HANDSHAKE | MESH_UI_UPDATE_MESSAGES);
    return 0;
}
