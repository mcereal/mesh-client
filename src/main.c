#include "mesh/app.h"
#include "mesh/config.h"
#include "mesh/log.h"
#include "mesh/transport/ble.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void print_handshake_json(FILE *out, const struct mesh_bluez_device_info *device,
                                 const struct mesh_ble_handshake_status *status,
                                 const struct mesh_message_log *log, bool cached);
static void print_handshake_pretty(FILE *out, const struct mesh_bluez_device_info *device,
                                   const struct mesh_ble_handshake_status *status,
                                   const struct mesh_message_log *log);
static void print_cached_handshake(FILE *out, const struct mesh_ui_handshake_state *state);
static void print_cached_handshake_json(FILE *out, const struct mesh_ui_handshake_state *state);
static int print_status(struct mesh_app *app, bool output_json, const char *output_path);
static int send_text_message(struct mesh_app *app, const char *text, uint32_t dest, uint8_t channel,
                             bool want_ack);
static bool parse_node_id(const char *value, uint32_t *out);
static void print_messages_pretty(FILE *out, const struct mesh_message_log *log);
static void print_messages_json(FILE *out, const struct mesh_message_log *log);
static void print_cached_messages(FILE *out, const struct mesh_ui_message_list *messages);
static void print_cached_messages_json(FILE *out, const struct mesh_ui_message_list *messages);
static const struct mesh_bluez_device_info *
select_preferred_device(const struct mesh_transport *ble, const struct mesh_app_config *config,
                        struct mesh_bluez_device_info *scratch, size_t *count);
static void json_print_string(FILE *out, const char *value);

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  -f, --foreground           Run until stopped (default: single poll)\n"
            "  -d, --disable-ble          Disable the BLE transport\n"
            "  -p, --preferred-device ID  Preferred BLE device address or name\n"
            "  -t, --timeout MS           Poll timeout in milliseconds (default: 1000)\n"
            "  -l, --log-level LEVEL      Log level (trace, debug, info, warn, error)\n"
            "  -s, --status              Connect to a device and print handshake summary\n"
            "      --json                Emit JSON instead of human-readable output (use with "
            "--status)\n"
            "      --status-output PATH  Write handshake JSON to PATH (implies --status --json)\n"
            "      --send-text TEXT      Connect and send a text message, then exit\n"
            "      --dest ID             Destination for --send-text: !hex, 0xhex, decimal, or\n"
            "                            'all' to broadcast (default: all)\n"
            "      --channel N           Channel index for --send-text (default: 0)\n"
            "      --ack                 Request delivery confirmation and wait for it\n"
            "                            (direct messages only; the mesh never acks broadcasts)\n"
            "  -h, --help                 Show this help message\n",
            program);
}

static enum mesh_log_level parse_log_level(const char *value, enum mesh_log_level fallback) {
    if (value == NULL) {
        return fallback;
    }

    if (strcasecmp(value, "trace") == 0) {
        return MESH_LOG_LEVEL_TRACE;
    }
    if (strcasecmp(value, "debug") == 0) {
        return MESH_LOG_LEVEL_DEBUG;
    }
    if (strcasecmp(value, "info") == 0) {
        return MESH_LOG_LEVEL_INFO;
    }
    if (strcasecmp(value, "warn") == 0 || strcasecmp(value, "warning") == 0) {
        return MESH_LOG_LEVEL_WARN;
    }
    if (strcasecmp(value, "error") == 0) {
        return MESH_LOG_LEVEL_ERROR;
    }

    mesh_log_warn("main", "Unknown log level '%s', keeping %s", value,
                  mesh_log_level_to_string(fallback));
    return fallback;
}

int main(int argc, char **argv) {
    struct mesh_app_config config = mesh_app_config_default();
    mesh_app_config_apply_env_overrides(&config);
    bool list_devices = false;
    bool show_status = false;
    bool output_json = false;
    const char *status_output_path = NULL;
    const char *send_text = NULL;
    uint32_t send_dest = MESH_MESSAGE_BROADCAST_ADDR;
    unsigned long send_channel = 0UL;
    bool send_want_ack = false;

    static const struct option long_options[] = {
        {"foreground", no_argument, NULL, 'f'},
        {"disable-ble", no_argument, NULL, 'd'},
        {"preferred-device", required_argument, NULL, 'p'},
        {"timeout", required_argument, NULL, 't'},
        {"log-level", required_argument, NULL, 'l'},
        {"list-devices", no_argument, NULL, 1},
        {"status", no_argument, NULL, 's'},
        {"json", no_argument, NULL, 'j'},
        {"status-output", required_argument, NULL, 2},
        {"send-text", required_argument, NULL, 3},
        {"dest", required_argument, NULL, 4},
        {"channel", required_argument, NULL, 5},
        {"ack", no_argument, NULL, 6},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    int option_index = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "fdp:t:l:hsj", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'f':
            config.run_mode = MESH_APP_RUN_FOREGROUND;
            break;
        case 'd':
            config.enable_ble = false;
            break;
        case 'p':
            if (optarg != NULL) {
                strncpy(config.preferred_ble_device, optarg,
                        sizeof(config.preferred_ble_device) - 1U);
                config.preferred_ble_device[sizeof(config.preferred_ble_device) - 1U] = '\0';
            }
            break;
        case 't':
            if (optarg != NULL) {
                config.idle_timeout_ms = atoi(optarg);
            }
            break;
        case 'l':
            mesh_log_set_level(parse_log_level(optarg, mesh_log_get_level()));
            break;
        case 1:
            list_devices = true;
            break;
        case 's':
            show_status = true;
            break;
        case 'j':
            output_json = true;
            break;
        case 2:
            status_output_path = optarg;
            show_status = true;
            break;
        case 3:
            send_text = optarg;
            break;
        case 4:
            if (!parse_node_id(optarg, &send_dest)) {
                fprintf(stderr, "Invalid --dest '%s'\n", optarg != NULL ? optarg : "");
                return EXIT_FAILURE;
            }
            break;
        case 5:
            if (optarg != NULL) {
                char *end = NULL;
                errno = 0;
                send_channel = strtoul(optarg, &end, 10);
                if (errno != 0 || end == optarg || *end != '\0' || send_channel > 7UL) {
                    fprintf(stderr, "Invalid --channel '%s' (expected 0-7)\n", optarg);
                    return EXIT_FAILURE;
                }
            }
            break;
        case 6:
            send_want_ack = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (status_output_path != NULL && status_output_path[0] == '\0') {
        status_output_path = NULL;
    }

    if (status_output_path != NULL) {
        output_json = true;
    }

    struct mesh_app app;
    int result = mesh_app_init(&app, &config);
    if (result < 0) {
        mesh_log_error("main", "Failed to initialise mesh client: %d", result);
        return EXIT_FAILURE;
    }

    if (send_text != NULL && send_text[0] == '\0') {
        fprintf(stderr, "--send-text requires a non-empty message\n");
        mesh_app_shutdown(&app);
        return EXIT_FAILURE;
    }

    if (list_devices || show_status || send_text != NULL) {
        result = mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop);
        if (result < 0) {
            mesh_log_error("main", "Failed to start transports: %d", result);
        } else if (list_devices) {
            struct mesh_transport *ble = mesh_ble_transport();
            mesh_ble_transport_refresh_devices(ble);
            size_t count = 0;
            const struct mesh_bluez_device_info *devices = mesh_ble_transport_devices(ble, &count);
            printf("Meshtastic BLE devices (%zu)\n", count);
            for (size_t i = 0; i < count; ++i) {
                printf("- %s (%s) RSSI=%d\n", devices[i].name, devices[i].address,
                       (int)devices[i].rssi);
            }
            mesh_transport_registry_stop_all(&app.transport_registry);
        } else if (send_text != NULL) {
            result =
                send_text_message(&app, send_text, send_dest, (uint8_t)send_channel, send_want_ack);
            mesh_transport_registry_stop_all(&app.transport_registry);
        } else {
            result = print_status(&app, output_json, status_output_path);
            mesh_transport_registry_stop_all(&app.transport_registry);
        }
    } else {
        result = mesh_app_run(&app);
        if (result < 0) {
            mesh_log_error("main", "mesh_app_run failed: %d", result);
        }
    }

    mesh_app_shutdown(&app);
    return (result < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}

static const struct mesh_bluez_device_info *
select_preferred_device(const struct mesh_transport *ble, const struct mesh_app_config *config,
                        struct mesh_bluez_device_info *scratch, size_t *count) {
    if (ble == NULL || config == NULL || scratch == NULL || count == NULL) {
        return NULL;
    }

    size_t device_count = mesh_ble_transport_refresh_devices((struct mesh_transport *)ble);
    const struct mesh_bluez_device_info *devices =
        mesh_ble_transport_devices((struct mesh_transport *)ble, &device_count);
    *count = device_count;
    if (device_count == 0U || devices == NULL) {
        return NULL;
    }

    if (device_count > 16U) {
        device_count = 16U;
        *count = device_count;
    }

    for (size_t i = 0; i < device_count; ++i) {
        scratch[i] = devices[i];
    }

    if (config->preferred_ble_device[0] != '\0') {
        for (size_t i = 0; i < device_count; ++i) {
            if (strcasecmp(scratch[i].address, config->preferred_ble_device) == 0 ||
                strcasecmp(scratch[i].name, config->preferred_ble_device) == 0) {
                return &scratch[i];
            }
        }
        mesh_log_warn("main", "Preferred device '%s' not found; falling back to strongest RSSI",
                      config->preferred_ble_device);
    }

    size_t best = 0U;
    for (size_t i = 1; i < device_count; ++i) {
        if (scratch[i].rssi > scratch[best].rssi) {
            best = i;
        }
    }
    return &scratch[best];
}

/* Connects to `target` and pumps the loop until the config handshake settles (or we give up).
   Shared by --status and --send-text: both need MyNodeInfo before their output means anything. */
static int connect_and_sync(struct mesh_app *app, struct mesh_transport *ble,
                            const struct mesh_bluez_device_info *target) {
    int connect_result = mesh_ble_transport_connect(ble, target->address);
    if (connect_result < 0 && connect_result != -EALREADY) {
        mesh_log_error("main", "Failed to connect to %s: %d", target->address, connect_result);
        return connect_result;
    }

    const int max_iterations = 50;
    for (int i = 0; i < max_iterations; ++i) {
        mesh_transport_registry_tick(&app->transport_registry);
        int run_result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
        if (run_result < 0) {
            mesh_log_warn("main", "Event loop returned error %d while waiting for handshake",
                          run_result);
            break;
        }

        struct mesh_ble_handshake_status status = mesh_ble_transport_handshake_status(ble);
        if (!status.request_in_flight && (status.config_complete || status.has_my_info)) {
            break;
        }
    }

    return 0;
}

static bool parse_node_id(const char *value, uint32_t *out) {
    if (value == NULL || value[0] == '\0' || out == NULL) {
        return false;
    }

    if (strcasecmp(value, "all") == 0 || strcasecmp(value, "broadcast") == 0) {
        *out = MESH_MESSAGE_BROADCAST_ADDR;
        return true;
    }

    /* Meshtastic writes node ids as "!433d1a2c"; accept that, 0x-prefixed hex, and decimal. */
    const char *digits = value;
    int base = 10;
    if (digits[0] == '!') {
        digits += 1;
        base = 16;
    } else if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits += 2;
        base = 16;
    }

    if (digits[0] == '\0') {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(digits, &end, base);
    if (errno != 0 || end == digits || *end != '\0' || parsed > 0xFFFFFFFFUL) {
        return false;
    }

    *out = (uint32_t)parsed;
    return true;
}

static int send_text_message(struct mesh_app *app, const char *text, uint32_t dest, uint8_t channel,
                             bool want_ack) {
    if (app == NULL || text == NULL) {
        return -EINVAL;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info devices[16];
    size_t device_count = 0U;
    const struct mesh_bluez_device_info *target =
        select_preferred_device(ble, &app->config, devices, &device_count);
    if (target == NULL) {
        fprintf(stderr, "No Meshtastic devices discovered; nothing to send through.\n");
        return -ENODEV;
    }

    int connect_result = connect_and_sync(app, ble, target);
    if (connect_result < 0) {
        return connect_result;
    }

    const bool broadcast = (dest == MESH_MESSAGE_BROADCAST_ADDR);
    if (want_ack && broadcast) {
        /* The mesh suppresses acks for broadcasts, so waiting for one would always time out. */
        fprintf(stderr, "Note: --ack is ignored for broadcasts.\n");
    }

    uint32_t packet_id = 0U;
    int send_result = mesh_ble_transport_send_text(ble, dest, channel, text, want_ack, &packet_id);
    if (send_result < 0) {
        mesh_log_error("main", "Failed to send message: %d", send_result);
        mesh_ble_transport_disconnect(ble);
        return send_result;
    }

    if (broadcast) {
        printf("Sent broadcast on channel %u (id=%u)\n", (unsigned)channel, packet_id);
    } else {
        printf("Sent to !%08x on channel %u (id=%u)\n", dest, (unsigned)channel, packet_id);
    }

    /* Pump the loop so the write actually reaches the radio, and so a requested ack has a
       chance to come back before we drop the link. */
    const int max_iterations = want_ack && !broadcast ? 50 : 5;
    enum mesh_message_ack final_ack = MESH_MESSAGE_ACK_NONE;
    for (int i = 0; i < max_iterations; ++i) {
        mesh_transport_registry_tick(&app->transport_registry);
        int run_result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
        if (run_result < 0) {
            mesh_log_warn("main", "Event loop returned error %d while sending", run_result);
            break;
        }

        const struct mesh_message_log *log = mesh_ble_transport_messages(ble);
        if (log == NULL) {
            break;
        }

        /* Read-only scan: mesh_message_log_find hands back a mutable entry for the ack path,
           which this caller has no business with. */
        for (size_t entry_index = 0; entry_index < log->count; ++entry_index) {
            const struct mesh_message *entry = mesh_message_log_at(log, entry_index);
            if (entry != NULL && entry->packet_id == packet_id) {
                final_ack = (enum mesh_message_ack)entry->ack;
            }
        }
        if (final_ack == MESH_MESSAGE_ACK_DELIVERED || final_ack == MESH_MESSAGE_ACK_FAILED) {
            break;
        }
    }

    int exit_result = 0;
    if (want_ack && !broadcast) {
        switch (final_ack) {
        case MESH_MESSAGE_ACK_DELIVERED:
            printf("Delivery confirmed.\n");
            break;
        case MESH_MESSAGE_ACK_FAILED:
            printf("Delivery failed (the mesh returned a routing error).\n");
            exit_result = -EIO;
            break;
        default:
            printf("No delivery confirmation within the wait window; the message may still "
                   "arrive.\n");
            break;
        }
    }

    mesh_ble_transport_disconnect(ble);
    return exit_result;
}

static int print_status(struct mesh_app *app, bool output_json, const char *output_path) {
    if (app == NULL) {
        return -EINVAL;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info devices[16];
    size_t device_count = 0U;
    const struct mesh_bluez_device_info *target =
        select_preferred_device(ble, &app->config, devices, &device_count);
    if (target == NULL) {
        printf("No Meshtastic devices discovered.\n");
        if (output_json) {
            fprintf(stdout, "{\"device\":null,\"handshake\":null");
            if (app->ui_store.handshake_valid && app->ui_store.handshake.cached) {
                fprintf(stdout, ",\"cached_handshake\":");
                print_cached_handshake_json(stdout, &app->ui_store.handshake);
            }
            fprintf(stdout, ",\"cached_messages\":");
            print_cached_messages_json(stdout, &app->ui_store.messages);
            fprintf(stdout, "}\n");
        } else {
            if (app->ui_store.handshake_valid && app->ui_store.handshake.cached) {
                printf("\nCached mesh status:\n");
                print_cached_handshake(stdout, &app->ui_store.handshake);
            }
            print_cached_messages(stdout, &app->ui_store.messages);
        }
        return 0;
    }

    int connect_result = connect_and_sync(app, ble, target);
    if (connect_result < 0) {
        return connect_result;
    }

    struct mesh_ble_handshake_status status = mesh_ble_transport_handshake_status(ble);
    const struct mesh_message_log *messages = mesh_ble_transport_messages(ble);

    FILE *file = stdout;
    FILE *output_file = NULL;
    if (output_path != NULL) {
        output_file = fopen(output_path, "w");
        if (output_file == NULL) {
            mesh_log_error("main", "Failed to open %s: %s", output_path, strerror(errno));
            mesh_ble_transport_disconnect(ble);
            return -errno;
        }
        print_handshake_json(output_file, target, &status, messages, false);
        fflush(output_file);
    }

    if (output_json) {
        print_handshake_json(file, target, &status, messages, false);
    } else {
        print_handshake_pretty(file, target, &status, messages);
    }

    if (output_file != NULL) {
        fclose(output_file);
    }

    mesh_ble_transport_disconnect(ble);
    return 0;
}

static void print_handshake_pretty(FILE *out, const struct mesh_bluez_device_info *device,
                                   const struct mesh_ble_handshake_status *status,
                                   const struct mesh_message_log *log) {
    fprintf(out, "Device: %s (%s) RSSI=%d\n", device->name, device->address, (int)device->rssi);
    if (status == NULL) {
        fprintf(out, "Handshake: unavailable\n");
        print_messages_pretty(out, log);
        return;
    }

    fprintf(out, "Handshake: request=%u, pending=%s, complete=%s", status->request_id,
            status->request_in_flight ? "yes" : "no", status->config_complete ? "yes" : "no");
    if (status->config_complete) {
        fprintf(out, " (id=%u)", status->config_complete_id);
    }
    fprintf(out, "\n");

    if (status->has_my_info) {
        fprintf(out, "MyNode: id=%u, nodedb=%u, reboot_count=%u\n", status->my_info.my_node_num,
                status->my_info.nodedb_count, status->my_info.reboot_count);
    } else {
        fprintf(out, "MyNode: pending\n");
    }

    if (status->node_count == 0U) {
        fprintf(out, "Nodes: none cached\n");
    } else {
        fprintf(out, "Nodes (%zu):\n", status->node_count);
        for (size_t i = 0; i < status->node_count; ++i) {
            const struct mesh_ble_node_summary *node = &status->nodes[i];
            fprintf(out, "  - id=%u", node->node_id);
            if (node->long_name[0] != '\0') {
                fprintf(out, " name=%s", node->long_name);
            }
            if (node->short_name[0] != '\0') {
                fprintf(out, " (short=%s)", node->short_name);
            }
            fprintf(out, " last_heard=%u snr=%.2f", node->last_heard, (double)node->snr);
            if (node->has_hops_away) {
                fprintf(out, " hops=%u", node->hops_away);
            }
            if (node->via_mqtt) {
                fprintf(out, " via_mqtt");
            }
            fprintf(out, "\n");
        }
    }

    print_messages_pretty(out, log);
}

static void print_cached_handshake(FILE *out, const struct mesh_ui_handshake_state *state) {
    if (out == NULL || state == NULL) {
        return;
    }

    fprintf(out, "Handshake: request=%u pending=%s complete=%s", state->request_id,
            state->request_in_flight ? "yes" : "no", state->config_complete ? "yes" : "no");
    if (state->config_complete) {
        fprintf(out, " (id=%u)", state->config_complete_id);
    }
    fprintf(out, "\n");

    if (state->has_my_info) {
        fprintf(out, "MyNode: id=%u, nodedb=%u, reboot_count=%u\n", state->my_info.node_num,
                state->my_info.nodedb_entries, state->my_info.reboot_count);
    }

    if (state->primary_channel[0] != '\0') {
        fprintf(out, "Primary channel: %s\n", state->primary_channel);
    }

    if (state->node_count == 0U) {
        fprintf(out, "Nodes: none cached\n");
        return;
    }

    fprintf(out, "Nodes (%u):\n", state->node_count);
    for (uint32_t i = 0; i < state->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
        const struct mesh_ui_node_summary *node = &state->nodes[i];
        if (node->node_id == 0U && node->long_name[0] == '\0' && node->short_name[0] == '\0') {
            continue;
        }
        fprintf(out, "  - id=%u", node->node_id);
        if (node->long_name[0] != '\0') {
            fprintf(out, " name=%s", node->long_name);
        }
        if (node->short_name[0] != '\0') {
            fprintf(out, " (short=%s)", node->short_name);
        }
        fprintf(out, " snr=%.2f last_heard=%u", (double)node->snr, node->last_heard);
        if (node->has_hops_away) {
            fprintf(out, " hops=%u", node->hops_away);
        }
        if (node->via_mqtt) {
            fprintf(out, " via_mqtt");
        }
        fprintf(out, "\n");
    }
}

static void print_cached_handshake_json(FILE *out, const struct mesh_ui_handshake_state *state) {
    if (out == NULL || state == NULL) {
        fputs("null", out);
        return;
    }

    fprintf(out, "{\"request_id\":%u,\"request_in_flight\":%s,\"config_complete\":%s",
            state->request_id, state->request_in_flight ? "true" : "false",
            state->config_complete ? "true" : "false");
    if (state->config_complete) {
        fprintf(out, ",\"config_complete_id\":%u", state->config_complete_id);
    }
    fprintf(out, ",\"cached\":true");

    if (state->has_my_info) {
        fprintf(out, ",\"my_node\":{\"id\":%u,\"nodedb_count\":%u,\"reboot_count\":%u}",
                state->my_info.node_num, state->my_info.nodedb_entries,
                state->my_info.reboot_count);
    } else {
        fprintf(out, ",\"my_node\":null");
    }

    if (state->primary_channel[0] != '\0') {
        fprintf(out, ",\"primary_channel\":");
        json_print_string(out, state->primary_channel);
    }

    fprintf(out, ",\"nodes\":[");
    for (uint32_t i = 0; i < state->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
        const struct mesh_ui_node_summary *node = &state->nodes[i];
        if (i > 0U) {
            fputc(',', out);
        }
        fprintf(out, "{\"id\":%u,\"long_name\":", node->node_id);
        json_print_string(out, node->long_name);
        fprintf(out, ",\"short_name\":");
        json_print_string(out, node->short_name);
        fprintf(out, ",\"snr\":%.2f,\"last_heard\":%u,\"via_mqtt\":%s", (double)node->snr,
                node->last_heard, node->via_mqtt ? "true" : "false");
        if (node->has_hops_away) {
            fprintf(out, ",\"hops_away\":%u", node->hops_away);
        }
        fputc('}', out);
    }
    fprintf(out, "]}");
}

static void json_print_string(FILE *out, const char *value) {
    if (value == NULL) {
        fprintf(out, "null");
        return;
    }
    fputc('"', out);
    for (const char *c = value; *c != '\0'; ++c) {
        switch (*c) {
        case '\\':
            fputs("\\\\", out);
            break;
        case '\"':
            fputs("\\\"", out);
            break;
        case '\b':
            fputs("\\b", out);
            break;
        case '\f':
            fputs("\\f", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if ((unsigned char)*c < 0x20U) {
                fprintf(out, "\\u%04x", (unsigned int)(unsigned char)*c);
            } else {
                fputc(*c, out);
            }
            break;
        }
    }
    fputc('"', out);
}

/* The offline view: what the last session persisted, with no radio in reach. Shaped from the
   UI snapshot rather than the transport ring, because that is what survives a restart. */
static void print_cached_messages(FILE *out, const struct mesh_ui_message_list *messages) {
    if (messages == NULL || messages->count == 0U) {
        return;
    }

    fprintf(out, "\nCached messages (%u", messages->count);
    if (messages->dropped > 0U) {
        fprintf(out, ", %u older discarded", messages->dropped);
    }
    fprintf(out, "):\n");

    for (uint32_t i = 0; i < messages->count && i < MESH_UI_MAX_MESSAGES; ++i) {
        const struct mesh_ui_message *message = &messages->entries[i];
        const bool outbound = (message->direction == MESH_MESSAGE_OUTBOUND);
        fprintf(out, "  %s %s ch%u", outbound ? "->" : "<-",
                message->peer_name[0] != '\0' ? message->peer_name : "?",
                (unsigned)message->channel);
        if (outbound && message->ack != MESH_MESSAGE_ACK_NONE) {
            fprintf(out, " [%s]", mesh_message_ack_to_string((enum mesh_message_ack)message->ack));
        }
        fprintf(out, ": %s\n", message->text);
    }
}

static void print_cached_messages_json(FILE *out, const struct mesh_ui_message_list *messages) {
    if (messages == NULL) {
        fprintf(out, "[]");
        return;
    }

    fputc('[', out);
    for (uint32_t i = 0; i < messages->count && i < MESH_UI_MAX_MESSAGES; ++i) {
        const struct mesh_ui_message *message = &messages->entries[i];
        if (i > 0U) {
            fputc(',', out);
        }
        fprintf(out, "{\"id\":%u,\"direction\":\"%s\",\"peer\":%u,\"channel\":%u",
                message->packet_id,
                message->direction == MESH_MESSAGE_OUTBOUND ? "outbound" : "inbound", message->peer,
                (unsigned)message->channel);
        fprintf(out, ",\"broadcast\":%s", message->broadcast ? "true" : "false");
        if (message->rx_time != 0U) {
            fprintf(out, ",\"rx_time\":%u", message->rx_time);
        }
        fprintf(out, ",\"peer_name\":");
        json_print_string(out, message->peer_name);
        fprintf(out, ",\"ack\":\"%s\"",
                mesh_message_ack_to_string((enum mesh_message_ack)message->ack));
        fprintf(out, ",\"text\":");
        json_print_string(out, message->text);
        fputc('}', out);
    }
    fputc(']', out);
}

static void print_messages_pretty(FILE *out, const struct mesh_message_log *log) {
    if (log == NULL || log->count == 0U) {
        fprintf(out, "Messages: none\n");
        return;
    }

    fprintf(out, "Messages (%zu", log->count);
    if (log->dropped > 0U) {
        fprintf(out, ", %u older discarded", log->dropped);
    }
    fprintf(out, "):\n");

    for (size_t i = 0; i < log->count; ++i) {
        const struct mesh_message *message = mesh_message_log_at(log, i);
        if (message == NULL) {
            continue;
        }

        const bool outbound = (message->direction == MESH_MESSAGE_OUTBOUND);
        if (outbound) {
            if (message->to == MESH_MESSAGE_BROADCAST_ADDR) {
                fprintf(out, "  -> all");
            } else {
                fprintf(out, "  -> !%08x", message->to);
            }
        } else {
            fprintf(out, "  <- !%08x", message->from);
        }

        fprintf(out, " ch%u", (unsigned)message->channel);
        if (message->rx_time != 0U) {
            fprintf(out, " t=%u", message->rx_time);
        }
        if (outbound && message->ack != MESH_MESSAGE_ACK_NONE) {
            fprintf(out, " [%s]", mesh_message_ack_to_string((enum mesh_message_ack)message->ack));
        }
        fprintf(out, ": %s\n", message->text);
    }
}

static void print_messages_json(FILE *out, const struct mesh_message_log *log) {
    if (log == NULL) {
        fprintf(out, "[]");
        return;
    }

    fputc('[', out);
    for (size_t i = 0; i < log->count; ++i) {
        const struct mesh_message *message = mesh_message_log_at(log, i);
        if (message == NULL) {
            continue;
        }
        if (i > 0U) {
            fputc(',', out);
        }

        const bool outbound = (message->direction == MESH_MESSAGE_OUTBOUND);
        fprintf(out, "{\"id\":%u,\"direction\":\"%s\",\"from\":%u,\"to\":%u,\"channel\":%u",
                message->packet_id, outbound ? "outbound" : "inbound", message->from, message->to,
                (unsigned)message->channel);
        fprintf(out, ",\"broadcast\":%s",
                message->to == MESH_MESSAGE_BROADCAST_ADDR ? "true" : "false");
        if (message->rx_time != 0U) {
            fprintf(out, ",\"rx_time\":%u", message->rx_time);
        }
        fprintf(out, ",\"snr\":%.2f", (double)message->rx_snr);
        if (message->has_hops_away) {
            fprintf(out, ",\"hops_away\":%u", message->hops_away);
        }
        fprintf(out, ",\"ack\":\"%s\"",
                mesh_message_ack_to_string((enum mesh_message_ack)message->ack));
        if (message->ack == MESH_MESSAGE_ACK_FAILED) {
            fprintf(out, ",\"ack_error\":%u", message->ack_error);
        }
        fprintf(out, ",\"text\":");
        json_print_string(out, message->text);
        fputc('}', out);
    }
    fputc(']', out);
}

static void print_handshake_json(FILE *out, const struct mesh_bluez_device_info *device,
                                 const struct mesh_ble_handshake_status *status,
                                 const struct mesh_message_log *log, bool cached) {
    fprintf(out, "{");
    fprintf(out, "\"device\":{");
    fprintf(out, "\"address\":");
    json_print_string(out, device->address);
    fprintf(out, ",\"name\":");
    json_print_string(out, device->name);
    fprintf(out, ",\"rssi\":%d},", (int)device->rssi);

    if (status == NULL) {
        fprintf(out, "\"handshake\":null,\"messages\":");
        print_messages_json(out, log);
        fprintf(out, ",\"cached\":%s}\n", cached ? "true" : "false");
        return;
    }

    fprintf(out, "\"handshake\":{");
    fprintf(out, "\"request_id\":%u,", status->request_id);
    fprintf(out, "\"request_in_flight\":%s,", status->request_in_flight ? "true" : "false");
    fprintf(out, "\"config_complete\":%s", status->config_complete ? "true" : "false");
    if (status->config_complete) {
        fprintf(out, ",\"config_complete_id\":%u", status->config_complete_id);
    }

    if (status->has_my_info) {
        fprintf(out, ",\"my_node\":{\"id\":%u,\"nodedb_count\":%u,\"reboot_count\":%u}",
                status->my_info.my_node_num, status->my_info.nodedb_count,
                status->my_info.reboot_count);
    } else {
        fprintf(out, ",\"my_node\":null");
    }

    fprintf(out, ",\"nodes\":[");
    for (size_t i = 0; i < status->node_count && i < MESH_BLE_MAX_NODE_SUMMARY; ++i) {
        if (i > 0U) {
            fputc(',', out);
        }
        const struct mesh_ble_node_summary *node = &status->nodes[i];
        fprintf(out, "{\"id\":%u,", node->node_id);
        fprintf(out, "\"long_name\":");
        json_print_string(out, node->long_name);
        fprintf(out, ",\"short_name\":");
        json_print_string(out, node->short_name);
        fprintf(out, ",\"last_heard\":%u,\"snr\":%.2f,\"via_mqtt\":%s", node->last_heard,
                (double)node->snr, node->via_mqtt ? "true" : "false");
        if (node->has_hops_away) {
            fprintf(out, ",\"hops_away\":%u", node->hops_away);
        }
        fputc('}', out);
    }
    fprintf(out, "]}");
    fprintf(out, ",\"messages\":");
    print_messages_json(out, log);
    fprintf(out, ",\"cached\":%s}\n", cached ? "true" : "false");
}
