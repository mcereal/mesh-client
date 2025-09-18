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

    fprintf(stderr, "[cli-ui] Handshake: nodes=%" PRIu32 ", request_in_flight=%s, config_complete=%s",
            snapshot->handshake.node_count, snapshot->handshake.request_in_flight ? "yes" : "no",
            snapshot->handshake.config_complete ? "yes" : "no");
    if (snapshot->handshake.my_short_name[0] != '\0') {
        fprintf(stderr, ", me=%s", snapshot->handshake.my_short_name);
    }
    if (snapshot->handshake.primary_channel[0] != '\0') {
        fprintf(stderr, ", channel=%s", snapshot->handshake.primary_channel);
    }
    fputc('\n', stderr);
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
