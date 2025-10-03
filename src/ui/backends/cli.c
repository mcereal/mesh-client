#include "mesh/ui/backends/cli.h"

#include "mesh/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void mesh_ui_backend_cli_print_devices(const struct mesh_ui_snapshot *snapshot) {
    fprintf(stderr, "[cli-ui] Devices (%zu)\n", snapshot->device_count);
    for (size_t i = 0; i < snapshot->device_count; ++i) {
        const struct mesh_ui_device *device = &snapshot->devices[i];
        fprintf(stderr, "  - %s (%s) RSSI=%d%s\n", device->name[0] != '\0' ? device->name : "<unknown>",
                device->identifier[0] != '\0' ? device->identifier : "<unknown>",
                (int)device->rssi, device->connected ? " [connected]" : "");
    }
}

static void mesh_ui_backend_cli_print_handshake(const struct mesh_ui_snapshot *snapshot) {
    if (!snapshot->handshake_valid) {
        fprintf(stderr, "[cli-ui] Handshake: not available\n");
        return;
    }

    const struct mesh_ui_handshake_state *handshake = &snapshot->handshake;

    fprintf(stderr,
            "[cli-ui] Handshake: nodes=%" PRIu32 ", request=%s(%u) config=%s" "(%u)\n",
            handshake->node_count, handshake->request_in_flight ? "pending" : "idle", handshake->request_id,
            handshake->config_complete ? "done" : "pending", handshake->config_complete_id);

    if (handshake->has_my_info) {
        fprintf(stderr,
                "[cli-ui]   my_node=%u short=%s nodedb=%u reboots=%u\n",
                handshake->my_info.node_num,
                handshake->my_short_name[0] != '\0' ? handshake->my_short_name : "<unset>",
                handshake->my_info.nodedb_entries, handshake->my_info.reboot_count);
    }

    if (handshake->primary_channel[0] != '\0') {
        fprintf(stderr, "[cli-ui]   channel=%s\n", handshake->primary_channel);
    }

    const uint32_t to_print = (handshake->node_count > MESH_UI_MAX_HANDSHAKE_NODES)
                                  ? MESH_UI_MAX_HANDSHAKE_NODES
                                  : handshake->node_count;
    for (uint32_t i = 0; i < to_print; ++i) {
        const struct mesh_ui_node_summary *node = &handshake->nodes[i];
        if (node->node_id == 0U && node->long_name[0] == '\0' && node->short_name[0] == '\0') {
            continue;
        }
        fprintf(stderr, "[cli-ui]   node %u: %s (%s) last=%u snr=%.2f",
                node->node_id,
                node->long_name[0] != '\0' ? node->long_name : "<long?>",
                node->short_name[0] != '\0' ? node->short_name : "<short?>",
                node->last_heard, (double)node->snr);
        if (node->has_hops_away) {
            fprintf(stderr, " hops=%u", node->hops_away);
        }
        if (node->via_mqtt) {
            fprintf(stderr, " via_mqtt");
        }
        fputc('\n', stderr);
    }
}

static int mesh_ui_backend_cli_init(void **state, void *userdata) {
    struct mesh_ui_backend_cli_context *context = NULL;
    if (userdata != NULL) {
        context = (struct mesh_ui_backend_cli_context *)userdata;
        memset(context, 0, sizeof *context);
    }
    if (state != NULL) {
        *state = context;
    }
    mesh_log_info("ui", "CLI UI backend active");
    return 0;
}

static void mesh_ui_backend_cli_shutdown(void *state, void *userdata) {
    (void)state;
    if (userdata != NULL) {
        struct mesh_ui_backend_cli_context *context = (struct mesh_ui_backend_cli_context *)userdata;
        memset(context, 0, sizeof *context);
    }
}

static void mesh_ui_backend_cli_present(void *state, const struct mesh_ui_snapshot *snapshot, void *userdata) {
    struct mesh_ui_backend_cli_context *context = NULL;
    if (state != NULL) {
        context = (struct mesh_ui_backend_cli_context *)state;
    } else if (userdata != NULL) {
        context = (struct mesh_ui_backend_cli_context *)userdata;
    }

    if (context == NULL || snapshot == NULL) {
        return;
    }

    context->updates_emitted++;
    context->last_snapshot = *snapshot;
    context->has_snapshot = true;

    if ((snapshot->update_flags & MESH_UI_UPDATE_DISCOVERY) != 0U) {
        mesh_ui_backend_cli_print_devices(snapshot);
    }
    if ((snapshot->update_flags & MESH_UI_UPDATE_HANDSHAKE) != 0U) {
        mesh_ui_backend_cli_print_handshake(snapshot);
    }
}

const struct mesh_ui_backend *mesh_ui_backend_cli(void) {
    static const struct mesh_ui_backend k_backend = {
        .name = "cli",
        .init = mesh_ui_backend_cli_init,
        .shutdown = mesh_ui_backend_cli_shutdown,
        .present = mesh_ui_backend_cli_present,
    };
    return &k_backend;
}
