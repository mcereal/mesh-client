#include "mesh/app.h"
#include "mesh/config.h"
#include "mesh/log.h"
#include "mesh/transport/ble.h"

#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

    static const struct option long_options[] = {
        {"foreground", no_argument, NULL, 'f'},
        {"disable-ble", no_argument, NULL, 'd'},
        {"preferred-device", required_argument, NULL, 'p'},
        {"timeout", required_argument, NULL, 't'},
        {"log-level", required_argument, NULL, 'l'},
        {"list-devices", no_argument, NULL, 1},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    int option_index = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "fdp:t:l:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'f':
                config.run_mode = MESH_APP_RUN_FOREGROUND;
                break;
            case 'd':
                config.enable_ble = false;
                break;
            case 'p':
                if (optarg != NULL) {
                    strncpy(config.preferred_ble_device, optarg, sizeof(config.preferred_ble_device) - 1U);
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
            case 'h':
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    struct mesh_app app;
    int result = mesh_app_init(&app, &config);
    if (result < 0) {
        mesh_log_error("main", "Failed to initialise mesh client: %d", result);
        return EXIT_FAILURE;
    }

    if (list_devices) {
        result = mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop);
        if (result < 0) {
            mesh_log_error("main", "Failed to start transports: %d", result);
        } else {
            struct mesh_transport *ble = mesh_ble_transport();
            mesh_ble_transport_refresh_devices(ble);
            size_t count = 0;
            const struct mesh_bluez_device_info *devices = mesh_ble_transport_devices(ble, &count);
            printf("Meshtastic BLE devices (%zu)\n", count);
            for (size_t i = 0; i < count; ++i) {
                printf("- %s (%s) RSSI=%d\n", devices[i].name, devices[i].address, (int)devices[i].rssi);
            }
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
