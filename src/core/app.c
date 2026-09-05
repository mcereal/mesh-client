#define _POSIX_C_SOURCE 200809L

/*
 * The process: one event loop, two transports, one link at a time.
 *
 * What is left here after the split is the part that owns lifetime rather than content -
 * bringing the loop, the transports and the UI backend up, routing a connect to the transport
 * that owns the row, deciding on its own which radio to reach for, and tearing it all down.
 * The three neighbouring files hang off the seams in app_internal.h.
 */

#include "app_internal.h"

#include "mesh/transport/ble.h"
#include "mesh/transport/serial.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/stub.h"
#include "mesh/ui/preferences.h"
#include "mesh/utils/env.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- link routing --------------------------------------------------------------------- */

/*
 * Two links, one session, one radio at a time. The Devices tab lists BLE advertisers and USB
 * ports together, so a connect has to be routed to the transport that owns the row, and taking
 * one link up drops the other.
 */

static struct mesh_transport *mesh_app_transport_for_kind(uint8_t kind) {
    return kind == (uint8_t)MESH_UI_DEVICE_SERIAL ? mesh_serial_transport() : mesh_ble_transport();
}

struct mesh_transport *mesh_app_active_transport(void) {
    struct mesh_transport *serial = mesh_serial_transport();
    struct mesh_transport *ble = mesh_ble_transport();
    if (serial != NULL && mesh_serial_transport_connected_port(serial) != NULL) {
        return serial;
    }
    if (ble != NULL && mesh_ble_transport_connected_address(ble) != NULL) {
        return ble;
    }
    if (serial != NULL && mesh_serial_transport_is_connecting(serial)) {
        return serial;
    }
    if (ble != NULL &&
        (mesh_ble_transport_is_connecting(ble) || mesh_ble_transport_is_pairing(ble))) {
        return ble;
    }
    return ble;
}

const char *mesh_app_connected_identifier(void) {
    const char *port = mesh_serial_transport_connected_port(mesh_serial_transport());
    if (port != NULL && port[0] != '\0') {
        return port;
    }
    const char *address = mesh_ble_transport_connected_address(mesh_ble_transport());
    return (address != NULL && address[0] != '\0') ? address : NULL;
}

bool mesh_app_link_connecting(void) {
    /* Pairing counts: it is the first half of a connect the user asked for, and auto-connect
       taking the serial link up underneath it would leave two transports on one session. */
    return mesh_ble_transport_is_connecting(mesh_ble_transport()) ||
           mesh_ble_transport_is_pairing(mesh_ble_transport()) ||
           mesh_serial_transport_is_connecting(mesh_serial_transport());
}

/* Drops whatever link is up or coming up, except the transport we are about to use. */
static void mesh_app_release_other_link(const struct mesh_transport *keep) {
    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_transport *serial = mesh_serial_transport();
    if (ble != keep &&
        (mesh_ble_transport_connected_address(ble) != NULL ||
         mesh_ble_transport_is_connecting(ble) || mesh_ble_transport_is_pairing(ble))) {
        /* A pairing left running would finish and then connect BLE on top of this link. */
        mesh_ble_transport_disconnect(ble);
    }
    if (serial != keep && (mesh_serial_transport_connected_port(serial) != NULL ||
                           mesh_serial_transport_is_connecting(serial))) {
        mesh_serial_transport_disconnect(serial);
    }
}

/* Connects `identifier` over the transport `kind` names, dropping the other link first.
   Returns what the transport's connect returned. */
int mesh_app_link_connect(struct mesh_app *app, const char *identifier, uint8_t kind) {
    struct mesh_transport *transport = mesh_app_transport_for_kind(kind);
    if (transport == NULL) {
        return -ENODEV;
    }

    mesh_app_release_other_link(transport);

    /* This becomes the node auto-connect goes back to. The two preferences are kept apart so
       unplugging a USB node does not erase which radio to look for over the air. */
    if (kind == (uint8_t)MESH_UI_DEVICE_SERIAL) {
        snprintf(app->config.preferred_serial_device, sizeof app->config.preferred_serial_device,
                 "%s", identifier);
        if (mesh_serial_transport_connected_port(transport) != NULL ||
            mesh_serial_transport_is_connecting(transport)) {
            mesh_serial_transport_disconnect(transport);
        }
        return mesh_serial_transport_connect(transport, identifier);
    }

    snprintf(app->config.preferred_ble_device, sizeof app->config.preferred_ble_device, "%s",
             identifier);
    if (mesh_ble_transport_connected_address(transport) != NULL ||
        mesh_ble_transport_is_connecting(transport) || mesh_ble_transport_is_pairing(transport)) {
        mesh_ble_transport_disconnect(transport);
    }
    /* A connect the user asked for pairs the node when it needs it; auto-connect's own
       attempts go through mesh_ble_transport_connect() and never raise a PIN prompt. */
    return mesh_ble_transport_connect_and_pair(transport, identifier);
}

/* Button presses arrive here from the evdev reader and go straight into the UI store's
   navigation model; the repaint happens on the store's eventfd in the same loop turn. */
void mesh_app_on_ui_key(void *userdata, enum mesh_ui_key key) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL) {
        return;
    }
    mesh_ui_controller_handle_key(&app->ui_controller, key);
}

static void mesh_app_select_cli(struct mesh_app *app, const struct mesh_ui_backend **backend,
                                void **userdata) {
    if (backend != NULL) {
        *backend = mesh_ui_backend_cli();
    }
    if (userdata != NULL) {
        *userdata = &app->ui_cli_context;
    }
}

static void mesh_app_select_stub(const struct mesh_ui_backend **backend, void **userdata) {
    if (backend != NULL) {
        *backend = mesh_ui_backend_stub();
    }
    if (userdata != NULL) {
        *userdata = NULL;
    }
}

static bool mesh_app_select_fb(struct mesh_app *app, const struct mesh_ui_backend **backend,
                               void **userdata) {
    if (!mesh_ui_backend_fb_is_available()) {
        return false;
    }

    if (backend != NULL) {
        *backend = mesh_ui_backend_fb();
    }
    if (userdata != NULL) {
        app->ui_fb_context.loop = &app->loop;
        *userdata = &app->ui_fb_context;
    }
    return true;
}

static const struct mesh_ui_backend *mesh_app_select_backend(struct mesh_app *app,
                                                             void **userdata) {
    if (userdata != NULL) {
        *userdata = NULL;
    }

    const char *requested = getenv("MESHCLIENT_UI_BACKEND");
    if (requested != NULL && requested[0] == '\0') {
        requested = NULL;
    }

    const struct mesh_ui_backend *backend = NULL;
    void *backend_userdata = NULL;

    /* "cli" and "stub" are asked for explicitly; everything else - including no request at all -
       resolves to the framebuffer, which is what the pak runs, and falls back to the CLI backend
       only where there is no /dev/fb0 to draw on (a container, or a dev host). */
    if (requested != NULL && strcasecmp(requested, "cli") == 0) {
        mesh_app_select_cli(app, &backend, &backend_userdata);
    } else if (requested != NULL && strcasecmp(requested, "stub") == 0) {
        mesh_app_select_stub(&backend, &backend_userdata);
    } else {
        if (requested != NULL && strcasecmp(requested, "fb") != 0 &&
            strcasecmp(requested, "auto") != 0) {
            mesh_log_warn("ui", "Unknown UI backend '%s'; using the default", requested);
        }
        if (!mesh_app_select_fb(app, &backend, &backend_userdata)) {
            mesh_app_select_cli(app, &backend, &backend_userdata);
        }
    }

    if (backend == NULL) {
        mesh_app_select_cli(app, &backend, &backend_userdata);
    }

    if (userdata != NULL) {
        *userdata = backend_userdata;
    }

    return backend;
}

#define MESH_APP_AUTOCONNECT_RETRY_MS 2000U
#define MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS 60000U
/* How long a saved preferred node gets to show up in discovery before another node is used. */
#define MESH_APP_AUTOCONNECT_PREFERRED_GRACE_MS 30000U

/* Exponential backoff, shared by the two ways a connect can fail: the errno connect() handed
   back, and the failure that only surfaces later from tick(). Returns the delay it scheduled. */
static uint64_t mesh_app_backoff_autoconnect(struct mesh_app *app) {
    if (app->autoconnect_failures < 8U) {
        app->autoconnect_failures++;
    }
    uint64_t delay = (uint64_t)MESH_APP_AUTOCONNECT_RETRY_MS << (app->autoconnect_failures - 1U);
    if (delay > MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS) {
        delay = MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS;
    }
    app->autoconnect_retry_at_ms = mesh_time_monotonic_ms() + delay;
    return delay;
}

void mesh_app_autoconnect(struct mesh_app *app) {
    if (app == NULL || app->autoconnect_disabled || app->autoconnect_held ||
        app->config.run_mode != MESH_APP_RUN_FOREGROUND) {
        return;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    const bool link_up = (mesh_app_connected_identifier() != NULL);
    if (link_up) {
        /* An established link is the only proof an attempt worked, so it is the only thing that
           clears the backoff. */
        app->autoconnect_failures = 0U;
    }
    if (ble == NULL || link_up || mesh_app_link_connecting()) {
        return;
    }

    uint64_t now = mesh_time_monotonic_ms();
    if (app->autoconnect_started_ms == 0U) {
        app->autoconnect_started_ms = now;
    }
    if (now < app->autoconnect_retry_at_ms) {
        return;
    }

    /*
     * A plugged-in node wins over anything on the air: it needs no pairing, has no range to
     * lose, and is almost certainly why the cable is there. BLE keeps its own policy below for
     * when nothing is plugged in.
     */
    struct mesh_serial_device_info ports[MESH_SERIAL_MAX_DEVICES];
    struct mesh_transport *serial = mesh_serial_transport();
    const size_t port_count =
        mesh_serial_transport_get_devices(serial, ports, MESH_SERIAL_MAX_DEVICES);
    if (port_count > 0U) {
        const struct mesh_serial_device_info *port = &ports[0];
        const char *preferred_port = app->config.preferred_serial_device;
        if (preferred_port[0] != '\0') {
            for (size_t i = 0; i < port_count; ++i) {
                if (strcmp(ports[i].id, preferred_port) == 0 ||
                    (ports[i].path[0] != '\0' && strcmp(ports[i].path, preferred_port) == 0)) {
                    port = &ports[i];
                    break;
                }
            }
        }
        const char *identifier = port->path[0] != '\0' ? port->path : port->id;
        const int serial_result =
            mesh_app_link_connect(app, identifier, (uint8_t)MESH_UI_DEVICE_SERIAL);
        if (serial_result == 0 || serial_result == -EALREADY || serial_result == -EINPROGRESS) {
            if (serial_result == 0) {
                mesh_log_info("app", "Auto-connecting to %s over USB (%s)", port->name, identifier);
            }
            /* Not a success yet: the handshake still has to go out. The counter stays where it
               is until a link is actually up. */
            app->autoconnect_retry_at_ms = now + MESH_APP_AUTOCONNECT_RETRY_MS;
            return;
        }
        mesh_log_warn("app", "Auto-connect to %s over USB failed (%d); trying Bluetooth",
                      identifier, serial_result);
    }

    struct mesh_bluez_device_info devices[MESH_UI_MAX_DEVICES];
    size_t device_count = mesh_ble_transport_get_devices(ble, devices, MESH_UI_MAX_DEVICES);
    if (device_count == 0U) {
        return; /* nothing in range yet; discovery keeps running */
    }

    const struct mesh_bluez_device_info *target = NULL;
    const char *preferred = app->config.preferred_ble_device;
    if (preferred[0] != '\0') {
        for (size_t i = 0; i < device_count; ++i) {
            if (strcasecmp(devices[i].address, preferred) == 0 ||
                strcasecmp(devices[i].name, preferred) == 0) {
                target = &devices[i];
                break;
            }
        }
        if (target == NULL &&
            now - app->autoconnect_started_ms < MESH_APP_AUTOCONNECT_PREFERRED_GRACE_MS) {
            if (!app->autoconnect_waiting_logged) {
                mesh_log_info("app", "Preferred device '%s' not in range yet; waiting", preferred);
                app->autoconnect_waiting_logged = true;
            }
            app->autoconnect_retry_at_ms = now + 1000U;
            return;
        }
    }

    if (target == NULL) {
        size_t best = 0U;
        for (size_t i = 1; i < device_count; ++i) {
            if (devices[i].rssi > devices[best].rssi) {
                best = i;
            }
        }
        target = &devices[best];
        mesh_log_info("app", "%s; using strongest node %s (%s, %d dBm)",
                      preferred[0] != '\0' ? "Preferred device not in range"
                                           : "No preferred device saved",
                      target->name, target->address, (int)target->rssi);
    }

    int result = mesh_ble_transport_connect(ble, target->address);
    if (result == 0 || result == -EALREADY || result == -EINPROGRESS) {
        if (result == 0) {
            mesh_log_info("app", "Auto-connecting to %s (%s)", target->name, target->address);
        }
        /* Not a success yet: BLE only resolves its services a few seconds from now. */
        app->autoconnect_retry_at_ms = now + MESH_APP_AUTOCONNECT_RETRY_MS;
        return;
    }
    if (result == -EAGAIN) {
        app->autoconnect_retry_at_ms = now + 1000U; /* transport not READY yet */
        return;
    }

    const uint64_t delay = mesh_app_backoff_autoconnect(app);
    mesh_log_warn("app", "Auto-connect to %s failed (%d); retrying in %llu ms", target->address,
                  result, (unsigned long long)delay);
}

bool mesh_app_report_link_errors(struct mesh_app *app) {
    if (app == NULL) {
        return false;
    }

    char link_error[MESH_TRANSPORT_ERROR_MAX];
    if (!mesh_transport_registry_take_error(&app->transport_registry, link_error,
                                            sizeof link_error)) {
        return false;
    }

    if (app->ui_report_link_error && app->config.run_mode == MESH_APP_RUN_FOREGROUND) {
        mesh_log_info("ui", "Link failure shown to the user: %s", link_error);
        mesh_ui_store_set_toast(&app->ui_store, mesh_time_monotonic_ms(), link_error);
    } else {
        mesh_log_debug("ui", "Link failure not shown (auto-connect): %s", link_error);
    }
    app->ui_report_link_error = false;

    /*
     * This is also the only honest failure signal auto-connect has. Its backoff keys off what
     * connect() returned, and a BLE connect returns 0 several seconds before it is a
     * connection - so a node that refuses every attempt looked like an unbroken run of
     * successes and got hammered every couple of seconds forever.
     */
    if (mesh_app_connected_identifier() == NULL) {
        (void)mesh_app_backoff_autoconnect(app);
    }
    return true;
}

int mesh_app_init(struct mesh_app *app, const struct mesh_app_config *config) {
    if (app == NULL) {
        return -EINVAL;
    }

    if (config != NULL) {
        app->config = *config;
    } else {
        app->config = mesh_app_config_default();
        mesh_app_config_apply_env_overrides(&app->config);
    }

    int result = mesh_event_loop_init(&app->loop);
    if (result < 0) {
        mesh_log_error("app", "Event loop init failed: %d", result);
        return result;
    }

    memset(&app->ui_fb_context, 0, sizeof app->ui_fb_context);
    memset(&app->ui_input, 0, sizeof app->ui_input);
    memset(&app->signals, 0, sizeof app->signals);
    app->signals.fd = -1;
    memset(&app->ui_preferences, 0, sizeof(app->ui_preferences));
    app->ui_preferences_path[0] = '\0';
    app->ui_preferences_dirty = false;
    app->ui_handshake_cache_path[0] = '\0';
    app->autoconnect_started_ms = 0U;
    app->autoconnect_retry_at_ms = 0U;
    app->autoconnect_failures = 0U;
    app->autoconnect_waiting_logged = false;
    app->ui_link_was_connected = false;
    app->ui_report_link_error = false;
    app->autoconnect_disabled = !mesh_env_bool("MESHCLIENT_AUTOCONNECT", "auto-connect", true);
    if (app->autoconnect_disabled) {
        mesh_log_info("app", "Auto-connect disabled by MESHCLIENT_AUTOCONNECT");
    }
    app->ui_handshake_cache_dirty = false;
    app->ui_read_state_stamp = 0U;

    if (mesh_ui_preferences_default_path(app->ui_preferences_path,
                                         sizeof(app->ui_preferences_path)) == 0) {
        int load_result = mesh_ui_preferences_load(&app->ui_preferences, app->ui_preferences_path);
        if (load_result == 0) {
            if (app->ui_preferences.preferred_device[0] != '\0') {
                if (app->ui_preferences.preferred_device_kind == (uint8_t)MESH_UI_DEVICE_SERIAL) {
                    if (app->config.preferred_serial_device[0] == '\0') {
                        snprintf(app->config.preferred_serial_device,
                                 sizeof app->config.preferred_serial_device, "%s",
                                 app->ui_preferences.preferred_device);
                    }
                } else if (app->config.preferred_ble_device[0] == '\0') {
                    snprintf(app->config.preferred_ble_device,
                             sizeof app->config.preferred_ble_device, "%s",
                             app->ui_preferences.preferred_device);
                }
            }
        }
        int handshake_written =
            snprintf(app->ui_handshake_cache_path, sizeof(app->ui_handshake_cache_path),
                     "%s.handshake", app->ui_preferences_path);
        if (handshake_written < 0 ||
            handshake_written >= (int)sizeof(app->ui_handshake_cache_path)) {
            mesh_log_warn("app", "Handshake cache path truncated; disabling cache");
            app->ui_handshake_cache_path[0] = '\0';
        }
    }

    result = mesh_ui_store_init(&app->ui_store);
    if (result < 0) {
        mesh_log_error("app", "UI store init failed: %d", result);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }

    if (app->ui_handshake_cache_path[0] != '\0') {
        int handshake_load = mesh_ui_store_load(&app->ui_store, app->ui_handshake_cache_path);
        if (handshake_load < 0 && handshake_load != -ENOENT) {
            mesh_log_debug("app", "Failed to load handshake cache: %d", handshake_load);
        }
    }

    /* Keep the restored conversation aside: every publish merges it back in, so an empty
       transport log at startup never overwrites it. */
    app->ui_messages_cached = app->ui_store.messages;
    /* Restored read marks are already on disk; only later ones need a save. */
    app->ui_read_state_stamp = app->ui_store.read_state.stamp;

    void *backend_userdata = NULL;
    const struct mesh_ui_backend *ui_backend = mesh_app_select_backend(app, &backend_userdata);
    result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store, ui_backend,
                                     backend_userdata, &app->loop);
    if (result < 0) {
        mesh_log_warn("app", "UI backend init failed (%d); falling back to stub", result);
        result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store,
                                         mesh_ui_backend_stub(), NULL, &app->loop);
    }
    if (result < 0) {
        mesh_log_error("app", "UI controller init failed: %d", result);
        mesh_ui_store_shutdown(&app->ui_store);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }
    mesh_ui_controller_set_action_handler(&app->ui_controller, mesh_app_on_ui_action, app);

    /* Never fatal: a client that cannot update itself is still a working client, and the
       About section says why rather than offering a row that would do nothing. */
    (void)mesh_updater_init(&app->updater, &app->loop);
    /* After init, which zeroes the struct. A prefs file written before the setting existed
       reads as DEFAULT, so this is a no-op for anyone who has never picked a channel. */
    (void)mesh_updater_set_channel(&app->updater,
                                   (enum mesh_update_channel)app->ui_preferences.update_channel);
    /* Skipped when the environment already asked: an explicit override on the command line
       should not be quietly undone by a file written on some earlier run. */
    if (!app->updater.allow_dev_from_env) {
        (void)mesh_updater_set_allow_dev(&app->updater, app->ui_preferences.update_allow_dev);
    }

    /* Optional canned.txt next to the preferences file replaces the built-in quick replies. */
    if (app->ui_preferences_path[0] != '\0') {
        char canned_path[sizeof app->ui_preferences_path + 16U];
        snprintf(canned_path, sizeof canned_path, "%s", app->ui_preferences_path);
        char *slash = strrchr(canned_path, '/');
        if (slash != NULL) {
            const size_t room = sizeof canned_path - (size_t)(slash + 1 - canned_path);
            snprintf(slash + 1, room, "%s", "canned.txt");
            const int loaded = mesh_ui_canned_load(canned_path);
            if (loaded > 0) {
                mesh_log_info("app", "Loaded %d canned replies from %s", loaded, canned_path);
            } else if (loaded != -ENOENT) {
                mesh_log_warn("app", "Ignoring %s: %d", canned_path, loaded);
            }
        }
    }

    mesh_transport_registry_init(&app->transport_registry);

    result = mesh_transport_registry_register(&app->transport_registry, mesh_ble_transport());
    if (result < 0) {
        mesh_log_error("app", "Failed to register BLE transport: %d", result);
        mesh_ui_controller_shutdown(&app->ui_controller);
        mesh_ui_store_shutdown(&app->ui_store);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }

    result = mesh_transport_registry_register(&app->transport_registry, mesh_serial_transport());
    if (result < 0) {
        mesh_log_error("app", "Failed to register serial transport: %d", result);
        mesh_ui_controller_shutdown(&app->ui_controller);
        mesh_ui_store_shutdown(&app->ui_store);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }

    /* One conversation for both links, so switching between them keeps the message log. */
    mesh_session_init(&app->session);
    /* The roster goes back into the session, which owns it: the first publish after a connect
       replaces the store's copy wholesale, so anything left only in the store would be lost.
       After mesh_session_init, which clears the session it is seeding. */
    mesh_app_seed_nodes_from_cache(app);
    mesh_transport_registry_set_session(&app->transport_registry, &app->session);

    return 0;
}

void mesh_app_shutdown(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }

    mesh_transport_registry_stop_all(&app->transport_registry);
    /* The transports are process-wide singletons but the session lives in `app`; leaving them
       pointed at it would dangle for anything that uses a transport after this. */
    mesh_transport_registry_set_session(&app->transport_registry, NULL);
    mesh_ui_input_shutdown(&app->ui_input);
    mesh_signals_shutdown(&app->signals);
    /* Before the loop goes: the updater has an fd registered with it, and a half-finished
       download to clean up. */
    mesh_updater_shutdown(&app->updater);
    mesh_ui_controller_shutdown(&app->ui_controller);
    if (app->ui_handshake_cache_path[0] != '\0') {
        mesh_ui_store_save(&app->ui_store, app->ui_handshake_cache_path);
        app->ui_handshake_cache_dirty = false;
    }
    mesh_ui_store_shutdown(&app->ui_store);
    if (app->ui_preferences_dirty && app->ui_preferences_path[0] != '\0') {
        mesh_ui_preferences_save(&app->ui_preferences, app->ui_preferences_path);
        app->ui_preferences_dirty = false;
    }
    mesh_event_loop_shutdown(&app->loop);
}

int mesh_app_run(struct mesh_app *app) {
    if (app == NULL) {
        return -EINVAL;
    }

    int result =
        mesh_transport_registry_start_all(&app->transport_registry, &app->config, &app->loop);
    if (result < 0) {
        return result;
    }

    /* Only the interactive run takes these over: --status and --list-devices stay plain CLI
       tools that Ctrl-C kills outright. Neither is fatal if it fails - a client that cannot
       read buttons is still better than no client. */
    mesh_signals_init(&app->signals, &app->loop);
    mesh_ui_input_init(&app->ui_input, &app->loop);
    mesh_ui_input_set_handler(&app->ui_input, mesh_app_on_ui_key, app);

    mesh_app_publish_ui_state(app);

    switch (app->config.run_mode) {
    case MESH_APP_RUN_SINGLE_POLL:
        mesh_log_debug("app", "Running single poll with timeout %d ms",
                       app->config.idle_timeout_ms);
        mesh_app_publish_ui_state(app);
        result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
        if (result >= 0) {
            mesh_app_publish_ui_state(app);
        }
        break;
    case MESH_APP_RUN_FOREGROUND:
        mesh_log_info("app", "Starting foreground event loop (timeout %d ms)",
                      app->config.idle_timeout_ms);
        /* Paint the first frame before any transport work: the store already has a refresh
           queued, and a zero timeout drains what is ready without waiting for more. */
        mesh_event_loop_run(&app->loop, 0);
        while (true) {
            mesh_transport_registry_tick(&app->transport_registry);
            /* The updater's child is watched by the event loop; this only enforces its
               timeout and reaps a child whose exit the loop did not see. */
            mesh_updater_tick(&app->updater, mesh_time_monotonic_ms());
            /* Before auto-connect, not after: a retry starts the link over and clears the
               reason the last attempt failed. */
            (void)mesh_app_report_link_errors(app);
            mesh_app_autoconnect(app);
            mesh_app_publish_ui_state(app);
            result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
            if (result < 0) {
                break;
            }
            if (app->loop.stop_requested) {
                mesh_log_info("app", "Event loop stop requested");
                break;
            }
            mesh_app_publish_ui_state(app);
        }
        break;
    default:
        mesh_log_warn("app", "Unknown run mode %d, performing single poll", app->config.run_mode);
        mesh_transport_registry_tick(&app->transport_registry);
        mesh_app_publish_ui_state(app);
        result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
        if (result >= 0) {
            mesh_app_publish_ui_state(app);
        }
        break;
    }

    mesh_transport_registry_stop_all(&app->transport_registry);
    /* Only the transport line changes here. A full publish would see the stopped transport
       report no handshake and no node names, and that empty state is what mesh_app_shutdown()
       would then save as the cache. */
    {
        struct mesh_transport *ble = mesh_ble_transport();
        const char *status = (ble != NULL && ble->ops != NULL && ble->ops->status != NULL)
                                 ? ble->ops->status(ble)
                                 : "stopped";
        mesh_ui_store_set_transport_status(&app->ui_store, status != NULL ? status : "stopped");
    }

    mesh_ui_input_shutdown(&app->ui_input);
    mesh_signals_shutdown(&app->signals);
    return result;
}
