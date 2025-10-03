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
}

void mesh_ui_store_reset(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_store preserved = *store;
    memset(store, 0, sizeof *store);
    store->event_fd = preserved.event_fd;
    store->pending_flags = preserved.pending_flags;
}

int mesh_ui_store_event_fd(const struct mesh_ui_store *store) {
    if (store == NULL) {
        return -1;
    }
    return store->event_fd;
}

void mesh_ui_store_set_discovery(struct mesh_ui_store *store, const struct mesh_ui_device *devices, size_t count) {
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

void mesh_ui_store_set_handshake(struct mesh_ui_store *store, const struct mesh_ui_handshake_state *handshake) {
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
    const bool payload_changed = next_valid && (memcmp(&store->handshake, &next_state, sizeof(next_state)) != 0);

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
        memcpy(snapshot->devices, store->devices, store->device_count * sizeof(struct mesh_ui_device));
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

static int mesh_ui_store_save_handshake(FILE *file, const struct mesh_ui_handshake_state *handshake) {
    if (file == NULL || handshake == NULL) {
        return 0;
    }

    fprintf(file, "handshake_request=%u,%u\n", handshake->request_in_flight ? 1U : 0U, handshake->request_id);
    fprintf(file, "handshake_config=%u,%u,%u\n", handshake->config_complete ? 1U : 0U,
            handshake->config_complete_id, handshake->has_config ? 1U : 0U);
    fprintf(file, "handshake_mynode=%u,%u,%u,%u\n", handshake->has_my_info ? 1U : 0U,
            handshake->my_info.node_num, handshake->my_info.nodedb_entries, handshake->my_info.reboot_count);
    mesh_ui_store_escape_and_write(file, "handshake_channel", handshake->primary_channel);
    mesh_ui_store_escape_and_write(file, "handshake_my_short", handshake->my_short_name);
    fprintf(file, "handshake_nodes=%u\n", handshake->node_count);
    for (uint32_t i = 0; i < handshake->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
        const struct mesh_ui_node_summary *node = &handshake->nodes[i];
        fprintf(file, "node[%u]=%u,%u,%u,%f,%u,%u\n", i, node->node_id, node->last_heard, node->has_hops_away ? 1U : 0U,
                (double)node->snr, node->via_mqtt ? 1U : 0U, node->hops_away);
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

    char line[512];
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
            if (sscanf(value, "%u,%u,%u,%u", &has_my_info, &node_num, &nodedb, &reboot_count) == 4) {
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
        } else if (strncmp(key, "node[", 5) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "node[%u]", &index) == 1) {
                uint32_t node_id = 0U;
                uint32_t last_heard = 0U;
                unsigned int has_hops = 0U;
                double snr = 0.0;
                unsigned int via_mqtt = 0U;
                unsigned int hops = 0U;
                if (sscanf(value, "%u,%u,%u,%lf,%u,%u", &node_id, &last_heard, &has_hops, &snr, &via_mqtt, &hops) == 6) {
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
                snprintf(handshake.nodes[index].long_name, sizeof(handshake.nodes[index].long_name), "%s", value);
            }
        } else if (strncmp(key, "node_short[", 11) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "node_short[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES) {
                snprintf(handshake.nodes[index].short_name, sizeof(handshake.nodes[index].short_name), "%s", value);
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
        store->handshake = handshake;
        store->handshake_valid = true;
    } else {
        memset(&store->handshake, 0, sizeof store->handshake);
        store->handshake_valid = false;
    }

    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_HANDSHAKE);
    return 0;
}
