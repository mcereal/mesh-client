#define _POSIX_C_SOURCE 200809L

#include "mesh/core/app.h"
#include "mesh/core/config.h"
#include "mesh/core/firmware_fetch.h"
#include "mesh/core/firmware_install.h"
#include "mesh/core/version.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/serial.h"
#include "mesh/utils/array.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/*
 * The radio --status and --send-text are talking to, and the link they are talking over. Both
 * transports own a mesh_session, so everything past the connect is shared; only the connect,
 * the disconnect and how the peer is named differ.
 */
struct mesh_cli_peer {
    const char *transport; /* "ble" or "serial" */
    const char *name;
    const char *identifier; /* BLE address, or the tty path */
    bool has_rssi;          /* BLE only */
    int rssi;
};

struct mesh_cli_link {
    struct mesh_transport *transport;
    struct mesh_session *session;
    int (*connect)(struct mesh_transport *transport, const char *identifier);
    int (*disconnect)(struct mesh_transport *transport);
    struct mesh_cli_peer peer;
};

static void print_handshake_json(FILE *out, const struct mesh_cli_peer *device,
                                 const struct mesh_handshake_status *status,
                                 const struct mesh_message_log *log, bool cached);
static void print_handshake_pretty(FILE *out, const struct mesh_cli_peer *device,
                                   const struct mesh_handshake_status *status,
                                   const struct mesh_message_log *log);
static void print_cached_handshake(FILE *out, const struct mesh_ui_handshake_state *state);
static void print_cached_handshake_json(FILE *out, const struct mesh_ui_handshake_state *state);
static int print_status(struct mesh_app *app, const struct mesh_cli_link *link, bool output_json,
                        const char *output_path);
static int send_text_message(struct mesh_app *app, const struct mesh_cli_link *link,
                             const char *text, uint32_t dest, uint8_t channel, bool want_ack);
static int select_ble_link(struct mesh_app *app, struct mesh_bluez_device_info *scratch,
                           struct mesh_cli_link *link);
static size_t await_ble_discovery(struct mesh_app *app);
static int select_serial_link(struct mesh_app *app, const char *requested,
                              struct mesh_serial_device_info *scratch, size_t scratch_len,
                              struct mesh_cli_link *link);
static int connect_and_sync(struct mesh_app *app, const struct mesh_cli_link *link);
static bool parse_node_id(const char *value, uint32_t *out);
static void print_messages_pretty(FILE *out, const struct mesh_message_log *log);
static void print_messages_json(FILE *out, const struct mesh_message_log *log);
static void print_cached_messages(FILE *out, const struct mesh_ui_message_list *messages);
static void print_cached_messages_json(FILE *out, const struct mesh_ui_message_list *messages);
static const struct mesh_bluez_device_info *
select_preferred_device(const struct mesh_transport *ble, const struct mesh_app_config *config,
                        struct mesh_bluez_device_info *scratch, size_t *count, bool heard_any);
static void json_print_string(FILE *out, const char *value);

#define MESH_CLI_DISCOVERY_WAIT_MS 4000U
#define MESH_CLI_DISCOVERY_POLL_MS 250U

/*
 * Waits for the BLE scan to actually hear something, up to MESH_CLI_DISCOVERY_WAIT_MS.
 *
 * A one-shot command starts the transports and asks its question in the same breath, and
 * StartDiscovery only *starts* scanning - so the first GetManagedObjects lands before any
 * advertisement has arrived, and every device in it is a bond with no RSSI. Reading that as
 * "nothing is in range" would fail a --send-text that was about to work. This is also what
 * brings BlueZ up when the transport was not READY yet: mesh_ble_bring_up() retries from
 * tick(), so the loop is the same thing the foreground app does with its event loop.
 *
 * Returns how many nodes were heard, which is 0 when the wait ran out.
 */
static size_t await_ble_discovery(struct mesh_app *app) {
    struct mesh_transport *ble = mesh_ble_transport();
    if (ble == NULL) {
        return 0U;
    }

    struct mesh_bluez_device_info devices[16];
    for (unsigned waited = 0U;; waited += MESH_CLI_DISCOVERY_POLL_MS) {
        const size_t count = mesh_ble_transport_get_devices(ble, devices, MESH_ARRAY_LEN(devices));
        size_t heard = 0U;
        for (size_t i = 0; i < count; ++i) {
            if (devices[i].in_range) {
                ++heard;
            }
        }
        if (heard > 0U || waited >= MESH_CLI_DISCOVERY_WAIT_MS) {
            return heard;
        }

        const struct timespec nap = {.tv_sec = 0,
                                     .tv_nsec = (long)MESH_CLI_DISCOVERY_POLL_MS * 1000000L};
        (void)nanosleep(&nap, NULL);
        mesh_transport_registry_tick(&app->transport_registry);
        (void)mesh_ble_transport_refresh_devices(ble);
    }
}

/* --list-devices: both transports, so a USB node shows up next to the BLE advertisers. */
static void list_all_devices(struct mesh_app *app) {
    struct mesh_transport *ble = mesh_ble_transport();
    mesh_ble_transport_refresh_devices(ble);
    (void)await_ble_discovery(app);
    size_t count = 0U;
    const struct mesh_bluez_device_info *devices = mesh_ble_transport_devices(ble, &count);
    printf("Meshtastic BLE devices (%zu)\n", count);
    for (size_t i = 0; i < count; ++i) {
        /* A bonded node BlueZ has not heard in this scan has no RSSI to report, and printing
           the 0 that leaves behind reads as a signal so strong it is off the scale. */
        if (devices[i].in_range) {
            printf("- %s (%s) RSSI=%d%s\n", devices[i].name, devices[i].address,
                   (int)devices[i].rssi, devices[i].paired ? "" : " [needs pairing]");
        } else {
            printf("- %s (%s) [not in range]%s\n", devices[i].name, devices[i].address,
                   devices[i].paired ? "" : " [needs pairing]");
        }
    }

    struct mesh_transport *serial = mesh_serial_transport();
    const size_t serial_count = mesh_serial_transport_refresh_devices(serial);
    const struct mesh_serial_device_info *ports = mesh_serial_transport_devices(serial, NULL);
    printf("USB serial ports (%zu)\n", serial_count);
    for (size_t i = 0; i < serial_count && ports != NULL; ++i) {
        /* A port that cannot carry a session says so, exactly as the Devices tab badges it:
           this listing is where "why will that not connect" gets answered on the CLI. */
        printf("- %s (%04x:%04x) id=%s port=%s%s%s\n", ports[i].name, ports[i].vendor_id,
               ports[i].product_id, ports[i].id,
               ports[i].path[0] != '\0' ? ports[i].path : "(unbound)",
               ports[i].needs_line_state ? " [DTR via usbfs]" : "",
               mesh_serial_device_is_radio(&ports[i]) ? "" : " [in bootloader]");
    }
}

/*
 * --fetch-firmware: the whole of phase 2, run against the real service, with no radio involved.
 *
 * It exists because phase 2's promise - "0.5 MB fetched, it matches the CRC, and it inflates to
 * the image the manifest describes" - is not a thing a unit test can prove. The suite runs the
 * same chain against committed bytes through a fake CDN; this runs it against GitHub, from the
 * device, over the antenna the client will really use, which is where the range refusals, the
 * CA bundle and the busybox gzip actually live. It writes nothing to a radio and never will:
 * that is phase 3, and it starts from the file this leaves behind.
 *
 * The board is named by its build target rather than resolved from a connected radio, because
 * the two questions are separate and this is the one without a radio in it.
 */
struct cli_firmware_fetch {
    struct mesh_fetch *fetcher;
    struct mesh_firmware_fetch fetch;
    struct mesh_firmware_release release;
    const char *target;
    const char *staging;
    bool resolved;
    bool finished;
    bool ok;
};

static void cli_firmware_done(void *userdata, const struct mesh_firmware_fetch *fetch) {
    struct cli_firmware_fetch *const run = (struct cli_firmware_fetch *)userdata;
    run->finished = true;
    run->ok = fetch->state == MESH_FIRMWARE_FETCH_READY;
}

static void cli_firmware_index(void *userdata, const struct mesh_fetch_result *result) {
    struct cli_firmware_fetch *const run = (struct cli_firmware_fetch *)userdata;
    if (result->outcome != MESH_FETCH_OK || result->body == NULL ||
        !mesh_firmware_release_parse(result->body, result->len, MESH_FIRMWARE_CHANNEL_STABLE,
                                     &run->release)) {
        fprintf(stderr, "Could not read the release index.\n");
        run->finished = true;
        return;
    }
    if (run->release.manifest_url[0] == '\0') {
        /* A release can appear in the index before its assets do, and one whose newest asset is
           still a per-platform zip has no manifest at all. */
        fprintf(stderr, "Release %s has published no manifest yet.\n", run->release.version);
        run->finished = true;
        return;
    }
    printf("Newest stable: %s\n", run->release.version);
    run->resolved = true;
    if (mesh_firmware_fetch_start(&run->fetch, run->fetcher, run->target, run->release.version,
                                  run->release.manifest_url, "", run->staging, cli_firmware_done,
                                  run) != 0) {
        fprintf(stderr, "Could not start the fetch.\n");
        run->finished = true;
    }
}

/*
 * `run` is the caller's storage rather than a local, because the fetch's callbacks carry its
 * address as their userdata: an install reading the manifest and the staged path off a struct
 * copied out at the end would be reading one whose machine points at a dead frame.
 */
static int fetch_radio_firmware(struct mesh_app *app, struct cli_firmware_fetch *run,
                                const char *target, const char *staging) {
    if (run == NULL) {
        return -EINVAL;
    }
    memset(run, 0, sizeof *run);
    /* The updater's fetcher, because it is the one that already found the pak's CA bundle -
       the Brick has no system store, so without it every HTTPS request exits 60. */
    run->fetcher = &app->updater.fetch;
    run->target = target;
    run->staging = staging;

    if (!mesh_fetch_available(run->fetcher)) {
        fprintf(stderr, "No curl or wget on this device; nothing can be fetched.\n");
        return -ENOTSUP;
    }
    printf("Fetching firmware for %s into %s\n", target, staging);

    struct mesh_fetch_request request;
    memset(&request, 0, sizeof request);
    request.url = "https://api.meshtastic.org/github/firmware/list";
    request.timeout_ms = 30000U;
    /* The served index is 155 KB, almost all of it release notes, and there is no way to ask
       for less. */
    request.response_max = 512U * 1024U;
    request.on_done = cli_firmware_index;
    request.userdata = run;
    if (mesh_fetch_start(run->fetcher, &request, mesh_time_monotonic_ms()) != 0) {
        fprintf(stderr, "Could not start the release index fetch.\n");
        return -EIO;
    }

    unsigned last = 101U;
    enum mesh_firmware_fetch_state last_state = MESH_FIRMWARE_FETCH_STATE_COUNT;
    for (int turn = 0; turn < 60000 && !run->finished; ++turn) {
        (void)mesh_event_loop_run(&app->loop, 10);
        const uint64_t now = mesh_time_monotonic_ms();
        mesh_fetch_tick(run->fetcher, now);
        if (run->resolved) {
            mesh_firmware_fetch_tick(&run->fetch, now);
            const unsigned progress = mesh_firmware_fetch_progress(&run->fetch);
            /* On a change of step as well as of percentage: this makes two round trips through
               the same zip, and a bar alone cannot say which one is moving. */
            if ((run->fetch.state != last_state || progress != last) && progress % 10U == 0U) {
                printf("  %s %u%%\n", mesh_firmware_fetch_state_name(run->fetch.state), progress);
                last = progress;
                last_state = run->fetch.state;
            }
        }
    }

    if (!run->finished) {
        fprintf(stderr, "Timed out.\n");
        mesh_firmware_fetch_cancel(&run->fetch);
        return -ETIMEDOUT;
    }
    if (!run->ok) {
        fprintf(stderr, "Failed: %s\n",
                run->fetch.message[0] != '\0' ? run->fetch.message : "no release resolved");
        return -EIO;
    }

    char path[MESH_FETCH_PATH_MAX];
    printf("Image:    %s\n", run->fetch.image.name);
    printf("Size:     %llu bytes\n", (unsigned long long)run->fetch.image.bytes);
    printf("Staged:   %s\n",
           mesh_firmware_fetch_image_path(&run->fetch, path, sizeof path) != NULL ? path : "?");
    if (run->fetch.path == MESH_FIRMWARE_PATH_USB) {
        printf("UF2:      %u blocks, family %08x, %#x-%#x\n", (unsigned)run->fetch.uf2.blocks,
               (unsigned)run->fetch.uf2.family_id, (unsigned)run->fetch.uf2.first_address,
               (unsigned)run->fetch.uf2.last_address);
    }
    return 0;
}

/*
 * --install-firmware: phase 3, run against a real board.
 *
 * The whole handover in one command - fetch the image, ask the radio to go into DFU, wait for
 * its bootloader, write the blocks, watch it restart - because the parts of this that can only
 * be wrong on a Brick are not parts a suite can reach: the platform mounting the bootloader's
 * ghost FAT over /mnt/SDCARD, a full-speed USB link's real throughput, and a board that decides
 * for itself when it has had every block.
 *
 * The radio has to be on **USB**, which is the same constraint the feature has: an nRF52 over
 * BLE is phase 4's problem and phase 1's refusal row already says so. With no radio there at
 * all the install still runs - it waits for a bootloader instead of asking for one, which is
 * the double-tap path, and is also what a board too broken to be asked politely needs.
 */
struct cli_firmware_install {
    struct mesh_app *app;
    const struct mesh_cli_link *link;
    unsigned armed;
};

static int cli_install_arm(void *userdata) {
    struct cli_firmware_install *const run = (struct cli_firmware_install *)userdata;
    if (run->link == NULL || run->link->session == NULL) {
        return -ENOTCONN;
    }
    const int queued = mesh_session_radio_action(run->link->session, MESH_ADMIN_ENTER_DFU_MODE);
    if (queued < 0) {
        return queued;
    }
    run->armed += 1U;
    /* Queued is not sent. The admin queue drains from the session's tick, which the serial
       transport calls from its own - so the packet reaches the radio on these turns and not
       before. */
    for (int turn = 0; turn < 200; ++turn) {
        mesh_transport_registry_tick(&run->app->transport_registry);
        (void)mesh_event_loop_run(&run->app->loop, 10);
    }
    return 0;
}

static void cli_install_done(void *userdata, const struct mesh_firmware_install *install) {
    (void)userdata;
    (void)install;
}

static int install_radio_firmware(struct mesh_app *app, const char *target, const char *staging,
                                  const char *serial_identifier) {
    static struct cli_firmware_fetch fetched;
    const int got = fetch_radio_firmware(app, &fetched, target, staging);
    if (got < 0) {
        return got;
    }
    if (fetched.fetch.path != MESH_FIRMWARE_PATH_USB) {
        fprintf(stderr, "%s installs over Bluetooth, not USB; that is phase 4.\n", target);
        return -ENOTSUP;
    }
    /*
     * The family is looked up from the architecture the board's own manifest states rather than
     * read off the image, because reading it off the image is the check answering itself. In
     * the UI this comes from `deviceHardware` for the connected radio's hw_model, which is one
     * step better again - it is the board saying what it is rather than the archive.
     */
    const uint32_t family = mesh_uf2_family_for_architecture(fetched.fetch.manifest.architecture);
    if (family == 0U) {
        fprintf(stderr, "No UF2 family for architecture '%s'; this board has no USB path.\n",
                fetched.fetch.manifest.architecture);
        return -ENOTSUP;
    }

    char image_path[MESH_FETCH_PATH_MAX];
    if (mesh_firmware_fetch_image_path(&fetched.fetch, image_path, sizeof image_path) == NULL) {
        fprintf(stderr, "The image was fetched and then could not be found.\n");
        return -EIO;
    }

    int result =
        mesh_transport_registry_start_all(&app->transport_registry, &app->config, &app->loop);
    if (result < 0) {
        fprintf(stderr, "Could not start the transports: %d\n", result);
        return result;
    }

    struct mesh_serial_device_info serial_devices[MESH_SERIAL_MAX_DEVICES];
    struct mesh_cli_link link;
    memset(&link, 0, sizeof link);
    struct cli_firmware_install run;
    memset(&run, 0, sizeof run);
    run.app = app;

    const bool have_radio = select_serial_link(app, serial_identifier, serial_devices,
                                               MESH_SERIAL_MAX_DEVICES, &link) >= 0 &&
                            connect_and_sync(app, &link) >= 0;
    char port[64] = {0};
    if (have_radio) {
        run.link = &link;
        mesh_str_copy(port, sizeof port, link.peer.identifier);
        printf("Radio:    %s on %s\n", link.peer.name, port);
    } else {
        printf("No radio on USB. Double-tap the reset button to put the board in its "
               "bootloader.\n");
    }

    struct mesh_firmware_install install;
    memset(&install, 0, sizeof install);
    result = mesh_firmware_install_start(&install, &app->loop, image_path, port, family,
                                         have_radio ? cli_install_arm : NULL, &run,
                                         cli_install_done, &run);
    if (result < 0) {
        fprintf(stderr, "Could not start the install: %s\n",
                mesh_firmware_install_error_name(install.error));
        mesh_transport_registry_stop_all(&app->transport_registry);
        return result;
    }
    if (have_radio) {
        /* The verb is out and the radio is on its way down. A transport still holding a tty
           that is about to disappear only produces errors about a link nobody is using. */
        (void)link.disconnect(link.transport);
    }

    enum mesh_firmware_install_state last_state = MESH_FIRMWARE_INSTALL_STATE_COUNT;
    unsigned last_progress = 101U;
    while (mesh_firmware_install_busy(&install)) {
        (void)mesh_event_loop_run(&app->loop, 50);
        const uint64_t now = mesh_time_monotonic_ms();
        /* The registry keeps ticking on purpose: the radio's port disappearing is what the
           transport notices, and the client noticing is what phase 3's own "has it come back
           yet" is written against. */
        mesh_transport_registry_tick(&app->transport_registry);
        mesh_firmware_install_tick(&install, now);
        const unsigned progress = mesh_firmware_install_progress(&install);
        if (install.state != last_state || (progress != last_progress && progress % 5U == 0U)) {
            printf("  %s %u%%\n", mesh_firmware_install_state_name(install.state), progress);
            /* Thirteen seconds of write with the output block-buffered behind a pipe is a
               command that looks hung; `make deploy-run` is exactly that pipe. */
            fflush(stdout);
            last_state = install.state;
            last_progress = progress;
        }
    }

    const bool ok = install.state == MESH_FIRMWARE_INSTALL_DONE;
    if (ok) {
        printf("Installed. The board restarted into %s.\n", fetched.release.version);
    } else {
        fprintf(stderr, "Failed: %s\n", mesh_firmware_install_error_name(install.error));
    }
    mesh_firmware_install_cancel(&install);
    mesh_transport_registry_stop_all(&app->transport_registry);
    return ok ? 0 : -EIO;
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  -f, --foreground           Run until stopped (default: single poll)\n"
            "  -d, --disable-ble          Disable the BLE transport\n"
            "      --disable-serial       Disable the USB serial transport\n"
            "  -p, --preferred-device ID  Preferred BLE device address or name\n"
            "      --serial[=ID]          Use a USB serial port instead of BLE for --status\n"
            "                             and --send-text. ID is a sysfs interface id\n"
            "                             (1-1:1.1) or a device node (/dev/ttyUSB0);\n"
            "                             without one, the first port found is used\n"
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
            "      --fetch-firmware TARGET  Download the newest stable firmware image for a\n"
            "                            build target (heltec-mesh-node-t114), verify it and\n"
            "                            leave it staged. No radio is touched\n"
            "      --install-firmware TARGET  Fetch that image and write it to the radio over\n"
            "                            USB: DFU request, wait for the bootloader, write the\n"
            "                            blocks, watch it restart. Changes the radio\n"
            "      --staging DIR         Where the two above stage (default: /tmp; use\n"
            "                            /mnt/UDISK on a Brick, because a bootloader's drive\n"
            "                            gets mounted over /mnt/SDCARD)\n"
            "  -V, --version              Print the client version and exit\n"
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
    bool use_serial = false;
    const char *serial_identifier = NULL;
    const char *fetch_firmware_target = NULL;
    /* /mnt/UDISK on a Brick, deliberately not the SD card: on the USB path the
       bootloader's ghost drive is mounted over /mnt/SDCARD the moment the radio
       reboots, so an image staged there vanishes from its own path. */
    const char *fetch_firmware_staging = "/tmp";
    const char *install_firmware_target = NULL;

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
        {"serial", optional_argument, NULL, 7},
        {"disable-serial", no_argument, NULL, 8},
        {"fetch-firmware", required_argument, NULL, 9},
        {"staging", required_argument, NULL, 10},
        {"install-firmware", required_argument, NULL, 11},
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    int option_index = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "fdp:t:l:hsjV", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'f':
            config.run_mode = MESH_APP_RUN_FOREGROUND;
            break;
        case 'd':
            config.enable_ble = false;
            break;
        case 'p':
            if (optarg != NULL) {
                mesh_str_copy(config.preferred_ble_device, sizeof config.preferred_ble_device,
                              optarg);
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
        case 9:
            fetch_firmware_target = optarg;
            break;
        case 10:
            if (optarg != NULL) {
                fetch_firmware_staging = optarg;
            }
            break;
        case 11:
            install_firmware_target = optarg;
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
        case 7:
            use_serial = true;
            serial_identifier = optarg; /* NULL for a bare --serial */
            break;
        case 8:
            config.enable_serial = false;
            break;
        case 'V':
            printf("meshclient %s\n", mesh_version_string());
            return EXIT_SUCCESS;
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

    if (install_firmware_target != NULL) {
        /*
         * Transports *are* started here, unlike the fetch above: the DFU request goes down the
         * serial link the radio is already on. Serial is a cable rather than the antenna, so
         * the one-part-one-antenna rule the download obeys does not apply to it - and the
         * download has finished by the time anything is connected.
         */
        result = install_radio_firmware(&app, install_firmware_target, fetch_firmware_staging,
                                        serial_identifier);
        mesh_app_shutdown(&app);
        return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    if (fetch_firmware_target != NULL) {
        /* No transports started: this reaches the network and nothing else, which is also the
           shape phase 3 will want - the radio link goes *down* for a download, because the
           Brick's Wi-Fi and its Bluetooth are one part behind one antenna. */
        static struct cli_firmware_fetch run;
        result = fetch_radio_firmware(&app, &run, fetch_firmware_target, fetch_firmware_staging);
        mesh_app_shutdown(&app);
        return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    if (list_devices || show_status || send_text != NULL) {
        result = mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop);
        if (result < 0) {
            mesh_log_error("main", "Failed to start transports: %d", result);
        } else if (list_devices) {
            list_all_devices(&app);
            mesh_transport_registry_stop_all(&app.transport_registry);
        } else {
            /* The scratch arrays back the peer strings in `link`, so they outlive its use. */
            struct mesh_bluez_device_info ble_devices[16];
            struct mesh_serial_device_info serial_devices[MESH_SERIAL_MAX_DEVICES];
            struct mesh_cli_link link;
            memset(&link, 0, sizeof link);

            const int select_result =
                use_serial ? select_serial_link(&app, serial_identifier, serial_devices,
                                                MESH_SERIAL_MAX_DEVICES, &link)
                           : select_ble_link(&app, ble_devices, &link);

            if (send_text != NULL) {
                if (select_result < 0) {
                    fprintf(stderr, "No %s node available; nothing to send through.\n",
                            use_serial ? "USB serial" : "Meshtastic BLE");
                    result = select_result;
                } else {
                    result = send_text_message(&app, &link, send_text, send_dest,
                                               (uint8_t)send_channel, send_want_ack);
                }
            } else {
                const struct mesh_cli_link *selected = select_result < 0 ? NULL : &link;
                result = print_status(&app, selected, output_json, status_output_path);
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

/*
 * `heard_any` is what the discovery wait came back with. With something heard, only a node that
 * answered is a candidate - the same rule auto-connect follows, and for the same reason. With
 * nothing heard the filter is dropped rather than applied to an empty scan: a radio already
 * connected to a phone stops advertising, and a bond is then the only handle there is. One
 * command gets to try it and fail; what must not happen is a whole session spent on it, which
 * is why the foreground path does not have this fallback.
 */
static const struct mesh_bluez_device_info *
select_preferred_device(const struct mesh_transport *ble, const struct mesh_app_config *config,
                        struct mesh_bluez_device_info *scratch, size_t *count, bool heard_any) {
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
            if ((scratch[i].in_range || !heard_any) &&
                (strcasecmp(scratch[i].address, config->preferred_ble_device) == 0 ||
                 strcasecmp(scratch[i].name, config->preferred_ble_device) == 0)) {
                if (!scratch[i].in_range) {
                    mesh_log_warn("main", "Nothing heard in this scan; trying bonded device '%s'",
                                  config->preferred_ble_device);
                }
                return &scratch[i];
            }
        }
        mesh_log_warn("main", "Preferred device '%s' not in range; falling back to strongest RSSI",
                      config->preferred_ble_device);
    }

    /* Otherwise the loudest node that answered - never a bond with no reading behind it, whose
       absent RSSI reads as a raw 0 and so beats every real measurement, all of which are
       negative. See mesh_bluez_device_info.in_range. */
    const struct mesh_bluez_device_info *best = NULL;
    for (size_t i = 0; i < device_count; ++i) {
        if ((scratch[i].in_range || !heard_any) && (best == NULL || scratch[i].rssi > best->rssi)) {
            best = &scratch[i];
        }
    }
    return best;
}

/* Builds the BLE half of a CLI link. `scratch` must outlive the link: the peer names point
   into it. Returns 0, or -ENODEV when nothing was discovered. */
static int select_ble_link(struct mesh_app *app, struct mesh_bluez_device_info *scratch,
                           struct mesh_cli_link *link) {
    struct mesh_transport *ble = mesh_ble_transport();
    /* Discovery has only just been started, so give it a moment to hear something before
       asking which nodes are in range. */
    const bool heard_any = await_ble_discovery(app) > 0U;
    size_t device_count = 0U;
    const struct mesh_bluez_device_info *target =
        select_preferred_device(ble, &app->config, scratch, &device_count, heard_any);
    if (target == NULL) {
        return -ENODEV;
    }

    link->transport = ble;
    link->session = mesh_ble_transport_session(ble);
    link->connect = mesh_ble_transport_connect;
    link->disconnect = mesh_ble_transport_disconnect;
    link->peer.transport = "ble";
    link->peer.name = target->name;
    link->peer.identifier = target->address;
    link->peer.has_rssi = true;
    link->peer.rssi = (int)target->rssi;
    return 0;
}

/* Builds the serial half. `requested` is the --serial argument (NULL or empty means whatever
   --preferred-serial-device says, else the first port found). */
static int select_serial_link(struct mesh_app *app, const char *requested,
                              struct mesh_serial_device_info *scratch, size_t scratch_len,
                              struct mesh_cli_link *link) {
    struct mesh_transport *serial = mesh_serial_transport();
    mesh_serial_transport_refresh_devices(serial);
    const size_t count = mesh_serial_transport_get_devices(serial, scratch, scratch_len);
    if (count == 0U) {
        return -ENODEV;
    }

    /*
     * Named is honoured, chosen is filtered.
     *
     * `--serial <id>` names a port, and if that port is a node in its bootloader the right
     * answer is the connect's refusal with its reason - not a silent hop to some other radio
     * the user did not ask for. Everything else is the *client* choosing: the remembered
     * preferred port, and the bare "whatever is plugged in" default. Those choose the way
     * mesh_app_autoconnect() does, because they are the same decision, and a bootloader is not
     * a candidate for either. Without the split, one node in DFU ahead of a working radio in
     * the scan makes --status and --send-text fail with -ENOTSUP while the radio that would
     * have answered sits in the same list.
     */
    const bool named = requested != NULL && requested[0] != '\0';
    const char *const wanted = named ? requested : app->config.preferred_serial_device;
    const struct mesh_serial_device_info *target = NULL;

    if (wanted[0] != '\0') {
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(scratch[i].id, wanted) == 0 ||
                (scratch[i].path[0] != '\0' && strcmp(scratch[i].path, wanted) == 0)) {
                target = &scratch[i];
                break;
            }
        }
        if (named && target == NULL) {
            mesh_log_error("main", "No USB serial port matches '%s'", wanted);
            return -ENODEV;
        }
        if (!named && target != NULL && !mesh_serial_device_is_radio(target)) {
            /* The port we used last time is in its bootloader. A preference is not an
               instruction, so anything actually running firmware beats it. */
            target = NULL;
        }
    }

    if (target == NULL) {
        for (size_t i = 0; i < count; ++i) {
            if (mesh_serial_device_is_radio(&scratch[i])) {
                target = &scratch[i];
                break;
            }
        }
    }
    if (target == NULL) {
        mesh_log_error("main", "Every USB serial port is a node in its bootloader; "
                               "press reset on the board to run its firmware");
        return -ENODEV;
    }

    link->transport = serial;
    link->session = mesh_serial_transport_session(serial);
    link->connect = mesh_serial_transport_connect;
    link->disconnect = mesh_serial_transport_disconnect;
    link->peer.transport = "serial";
    link->peer.name = target->name;
    /* The tty is what the user recognises; before the bind there is only the sysfs id. */
    link->peer.identifier = target->path[0] != '\0' ? target->path : target->id;
    link->peer.has_rssi = false;
    link->peer.rssi = 0;
    return 0;
}

/* Connects the link and pumps the loop until the config handshake settles (or we give up).
   Shared by --status and --send-text: both need MyNodeInfo before their output means anything. */
static int connect_and_sync(struct mesh_app *app, const struct mesh_cli_link *link) {
    int connect_result = link->connect(link->transport, link->peer.identifier);
    if (connect_result < 0 && connect_result != -EALREADY) {
        mesh_log_error("main", "Failed to connect to %s: %d", link->peer.identifier,
                       connect_result);
        return connect_result;
    }

    const int max_iterations = 50;
    for (int i = 0; i < max_iterations; ++i) {
        mesh_transport_registry_tick(&app->transport_registry);
        /* Both links finish opening from tick(): BLE waits on service discovery, serial on the
           wake settle. Poll briefly at first so that costs milliseconds, not a full timeout. */
        const int timeout_ms =
            i < 3 && app->config.idle_timeout_ms > 100 ? 100 : app->config.idle_timeout_ms;
        int run_result = mesh_event_loop_run(&app->loop, timeout_ms);
        if (run_result < 0) {
            mesh_log_warn("main", "Event loop returned error %d while waiting for handshake",
                          run_result);
            break;
        }

        const struct mesh_handshake_status *status = mesh_session_handshake(link->session);
        if (status != NULL && !status->request_in_flight &&
            (status->config_complete || status->has_my_info)) {
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

static int send_text_message(struct mesh_app *app, const struct mesh_cli_link *link,
                             const char *text, uint32_t dest, uint8_t channel, bool want_ack) {
    if (app == NULL || link == NULL || text == NULL) {
        return -EINVAL;
    }

    int connect_result = connect_and_sync(app, link);
    if (connect_result < 0) {
        return connect_result;
    }

    const bool broadcast = (dest == MESH_MESSAGE_BROADCAST_ADDR);
    if (want_ack && broadcast) {
        /* The mesh suppresses acks for broadcasts, so waiting for one would always time out. */
        fprintf(stderr, "Note: --ack is ignored for broadcasts.\n");
    }

    uint32_t packet_id = 0U;
    int send_result =
        mesh_session_send_text(link->session, dest, channel, text, want_ack, &packet_id);
    if (send_result < 0) {
        mesh_log_error("main", "Failed to send message: %d", send_result);
        link->disconnect(link->transport);
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

        const struct mesh_message_log *log = mesh_session_messages(link->session);
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

    link->disconnect(link->transport);
    return exit_result;
}

static int print_status(struct mesh_app *app, const struct mesh_cli_link *link, bool output_json,
                        const char *output_path) {
    if (app == NULL) {
        return -EINVAL;
    }

    if (link == NULL) {
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

    int connect_result = connect_and_sync(app, link);
    if (connect_result < 0) {
        return connect_result;
    }

    const struct mesh_handshake_status *status = mesh_session_handshake(link->session);
    const struct mesh_message_log *messages = mesh_session_messages(link->session);

    FILE *file = stdout;
    FILE *output_file = NULL;
    if (output_path != NULL) {
        output_file = fopen(output_path, "w");
        if (output_file == NULL) {
            mesh_log_error("main", "Failed to open %s: %s", output_path, strerror(errno));
            link->disconnect(link->transport);
            return -errno;
        }
        print_handshake_json(output_file, &link->peer, status, messages, false);
        fflush(output_file);
    }

    if (output_json) {
        print_handshake_json(file, &link->peer, status, messages, false);
    } else {
        print_handshake_pretty(file, &link->peer, status, messages);
    }

    if (output_file != NULL) {
        fclose(output_file);
    }

    link->disconnect(link->transport);
    return 0;
}

static void print_handshake_pretty(FILE *out, const struct mesh_cli_peer *device,
                                   const struct mesh_handshake_status *status,
                                   const struct mesh_message_log *log) {
    fprintf(out, "Device: %s (%s) over %s", device->name, device->identifier, device->transport);
    if (device->has_rssi) {
        fprintf(out, " RSSI=%d", device->rssi);
    }
    fprintf(out, "\n");
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
            const struct mesh_node_summary *node = &status->nodes[i];
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

static void print_handshake_json(FILE *out, const struct mesh_cli_peer *device,
                                 const struct mesh_handshake_status *status,
                                 const struct mesh_message_log *log, bool cached) {
    fprintf(out, "{");
    fprintf(out, "\"device\":{");
    fprintf(out, "\"address\":");
    json_print_string(out, device->identifier);
    fprintf(out, ",\"name\":");
    json_print_string(out, device->name);
    fprintf(out, ",\"transport\":");
    json_print_string(out, device->transport);
    fprintf(out, ",\"rssi\":");
    if (device->has_rssi) {
        fprintf(out, "%d", device->rssi);
    } else {
        fprintf(out, "null");
    }
    fprintf(out, "},");

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
    for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (i > 0U) {
            fputc(',', out);
        }
        const struct mesh_node_summary *node = &status->nodes[i];
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
