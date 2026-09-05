#include "mesh/ui/backends/cli.h"

#include "mesh/core/message.h"
#include "mesh/utils/array.h"
#include "mesh/utils/log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void mesh_ui_backend_cli_write(struct mesh_ui_backend_cli_context *context, const char *fmt,
                                      ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    if (context != NULL && context->tty_stream != NULL) {
        va_list tty_args;
        va_start(tty_args, fmt);
        vfprintf(context->tty_stream, fmt, tty_args);
        va_end(tty_args);
        fflush(context->tty_stream);
    }
}

static void mesh_ui_backend_cli_print_transport(struct mesh_ui_backend_cli_context *context,
                                                const struct mesh_ui_snapshot *snapshot) {
    mesh_ui_backend_cli_write(context, "[cli-ui] Transport: %s\n",
                              snapshot->transport_status[0] != '\0' ? snapshot->transport_status
                                                                    : "starting");
}

static void mesh_ui_backend_cli_print_devices(struct mesh_ui_backend_cli_context *context,
                                              const struct mesh_ui_snapshot *snapshot) {
    mesh_ui_backend_cli_write(context, "[cli-ui] Devices (%zu)\n", snapshot->device_count);
    for (size_t i = 0; i < snapshot->device_count; ++i) {
        const struct mesh_ui_device *device = &snapshot->devices[i];
        /* Same three states the framebuffer rows carry: an unpaired BLE node is the one worth
           saying out loud, because connecting to it bonds first. */
        const char *badge = "";
        if (device->connected) {
            badge = " [connected]";
        } else if (device->busy) {
            badge = " [working]";
        } else if (device->kind == (uint8_t)MESH_UI_DEVICE_BLE && !device->paired) {
            badge = " [needs pairing]";
        }
        if (device->kind == (uint8_t)MESH_UI_DEVICE_SERIAL) {
            mesh_ui_backend_cli_write(
                context, "  - %s (%s) USB%s\n",
                device->name[0] != '\0' ? device->name : "<unknown>",
                device->identifier[0] != '\0' ? device->identifier : "<unknown>", badge);
        } else {
            mesh_ui_backend_cli_write(context, "  - %s (%s) RSSI=%d%s\n",
                                      device->name[0] != '\0' ? device->name : "<unknown>",
                                      device->identifier[0] != '\0' ? device->identifier
                                                                    : "<unknown>",
                                      (int)device->rssi, badge);
        }
    }
}

static void mesh_ui_backend_cli_print_handshake(struct mesh_ui_backend_cli_context *context,
                                                const struct mesh_ui_snapshot *snapshot) {
    if (!snapshot->handshake_valid) {
        mesh_ui_backend_cli_write(context, "[cli-ui] Handshake: not available\n");
        return;
    }

    const struct mesh_ui_handshake_state *handshake = &snapshot->handshake;

    mesh_ui_backend_cli_write(
        context, "[cli-ui] Handshake: nodes=%" PRIu32 ", request=%s(%u) config=%s(%u)\n",
        handshake->node_count, handshake->request_in_flight ? "pending" : "idle",
        handshake->request_id, handshake->config_complete ? "done" : "pending",
        handshake->config_complete_id);

    if (handshake->has_my_info) {
        mesh_ui_backend_cli_write(
            context, "[cli-ui]   my_node=%u short=%s nodedb=%u reboots=%u\n",
            handshake->my_info.node_num,
            handshake->my_short_name[0] != '\0' ? handshake->my_short_name : "<unset>",
            handshake->my_info.nodedb_entries, handshake->my_info.reboot_count);
    }

    if (handshake->primary_channel[0] != '\0') {
        mesh_ui_backend_cli_write(context, "[cli-ui]   channel=%s\n", handshake->primary_channel);
    }

    const uint32_t to_print = (handshake->node_count > MESH_UI_MAX_HANDSHAKE_NODES)
                                  ? MESH_UI_MAX_HANDSHAKE_NODES
                                  : handshake->node_count;
    for (uint32_t i = 0; i < to_print; ++i) {
        const struct mesh_ui_node_summary *node = &handshake->nodes[i];
        if (node->node_id == 0U && node->long_name[0] == '\0' && node->short_name[0] == '\0') {
            continue;
        }
        mesh_ui_backend_cli_write(context, "[cli-ui]   node %u: %s (%s) last=%u snr=%.2f",
                                  node->node_id,
                                  node->long_name[0] != '\0' ? node->long_name : "<long?>",
                                  node->short_name[0] != '\0' ? node->short_name : "<short?>",
                                  node->last_heard, (double)node->snr);
        if (node->has_hops_away) {
            mesh_ui_backend_cli_write(context, " hops=%u", node->hops_away);
        }
        if (node->via_mqtt) {
            mesh_ui_backend_cli_write(context, " via_mqtt");
        }
        mesh_ui_backend_cli_write(context, "\n");
    }
}

static void mesh_ui_backend_cli_print_messages(struct mesh_ui_backend_cli_context *context,
                                               const struct mesh_ui_snapshot *snapshot) {
    const struct mesh_ui_message_list *messages = &snapshot->messages;
    if (messages->count == 0U) {
        mesh_ui_backend_cli_write(context, "[cli-ui] Messages: none\n");
        return;
    }

    mesh_ui_backend_cli_write(context, "[cli-ui] Messages (%" PRIu32 ")", messages->count);
    if (messages->dropped > 0U) {
        mesh_ui_backend_cli_write(context, " (+%" PRIu32 " older discarded)", messages->dropped);
    }
    mesh_ui_backend_cli_write(context, "\n");

    for (uint32_t i = 0; i < messages->count && i < MESH_UI_MAX_MESSAGES; ++i) {
        const struct mesh_ui_message *message = &messages->entries[i];
        const bool outbound = (message->direction == MESH_MESSAGE_OUTBOUND);
        mesh_ui_backend_cli_write(context, "[cli-ui]   %s %s ch%u: %s", outbound ? "->" : "<-",
                                  message->peer_name[0] != '\0' ? message->peer_name : "?",
                                  (unsigned)message->channel, message->text);
        if (outbound && message->ack != MESH_MESSAGE_ACK_NONE) {
            mesh_ui_backend_cli_write(
                context, " [%s]", mesh_message_ack_to_string((enum mesh_message_ack)message->ack));
        }
        mesh_ui_backend_cli_write(context, "\n");
    }
}

static void mesh_ui_backend_cli_print_nav(struct mesh_ui_backend_cli_context *context,
                                          const struct mesh_ui_snapshot *snapshot) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char convo[MESH_UI_NAV_TARGET_NAME_MAX];
    mesh_ui_nav_conversation_name(nav, convo, sizeof convo);
    mesh_ui_backend_cli_write(context, "[cli-ui] Screen: %s row %u, to %s, showing %s%s%s%s%s\n",
                              mesh_ui_screen_name(nav->screen), nav->cursor[nav->screen],
                              nav->target_name, convo, nav->keyboard_open ? " [keyboard: " : "",
                              nav->keyboard_open ? nav->draft : "", nav->keyboard_open ? "]" : "",
                              nav->toast[0] != '\0' ? " | " : "");
    if (nav->toast[0] != '\0') {
        mesh_ui_backend_cli_write(context, "[cli-ui] %s\n", nav->toast);
    }
}

static int mesh_ui_backend_cli_init(void **state, void *userdata) {
    struct mesh_ui_backend_cli_context *context = NULL;
    if (userdata != NULL) {
        context = (struct mesh_ui_backend_cli_context *)userdata;
        memset(context, 0, sizeof *context);
    }
    if (context != NULL) {
        const char *console_candidates[] = {"/dev/tty0", "/dev/tty1", "/dev/tty", "/dev/console"};
        for (size_t i = 0; i < MESH_ARRAY_LEN(console_candidates); ++i) {
            const char *path = console_candidates[i];
            FILE *stream = fopen(path, "w");
            if (stream != NULL) {
                setvbuf(stream, NULL, _IONBF, 0);
                context->tty_stream = stream;
                mesh_log_info("ui", "CLI backend writing to %s", path);
                break;
            }
        }
        if (context->tty_stream == NULL) {
            mesh_log_warn("ui", "CLI backend could not open a console for output");
        }
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
        struct mesh_ui_backend_cli_context *context =
            (struct mesh_ui_backend_cli_context *)userdata;
        memset(context, 0, sizeof *context);
        if (context->tty_stream != NULL) {
            fclose(context->tty_stream);
            context->tty_stream = NULL;
        }
    }
}

static void mesh_ui_backend_cli_present(void *state, const struct mesh_ui_snapshot *snapshot,
                                        void *userdata) {
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

    if ((snapshot->update_flags & MESH_UI_UPDATE_TRANSPORT) != 0U) {
        mesh_ui_backend_cli_print_transport(context, snapshot);
    }
    if ((snapshot->update_flags & MESH_UI_UPDATE_DISCOVERY) != 0U) {
        mesh_ui_backend_cli_print_devices(context, snapshot);
    }
    if ((snapshot->update_flags & MESH_UI_UPDATE_HANDSHAKE) != 0U) {
        mesh_ui_backend_cli_print_handshake(context, snapshot);
    }
    if ((snapshot->update_flags & MESH_UI_UPDATE_MESSAGES) != 0U) {
        mesh_ui_backend_cli_print_messages(context, snapshot);
    }
    if ((snapshot->update_flags & MESH_UI_UPDATE_NAV) != 0U) {
        mesh_ui_backend_cli_print_nav(context, snapshot);
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
