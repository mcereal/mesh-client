#include "app_internal.h"

#include "inkwell/base/env.h"
#include "inkwell/base/log.h"
#include "mesh/i18n/strings.h"
#include "mesh/transport/serial.h"
#include "mesh/transport/tcp.h"
#include "mesh/ui/store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Which protocol a cable or a host speaks, found out by asking.
 *
 * BLE needs none of this: MeshCore and Meshtastic advertise different services, and the scan
 * that found a radio already knows which. A USB port or a TCP socket is bytes, and the only way
 * to learn what is behind it is to say something and see what comes back. Each protocol's
 * opening ignores the other's - Meshtastic's parser waits for 0x94 0xC3 and MeshCore's for '<'
 * - so asking in the wrong one costs a window and nothing else.
 *
 * Meshtastic is asked first because it is what most radios on a cable are, and a radio that
 * answered once is asked in that protocol first next time, so a MeshCore radio pays the window
 * once a run rather than on every reconnect.
 */

static struct mesh_transport *mesh_app_probe_transport(uint8_t kind) {
    return kind == (uint8_t)MESH_UI_DEVICE_TCP ? mesh_tcp_transport() : mesh_serial_transport();
}

/* MESHCLIENT_PROTOCOL, as a probe bit, or 0 when it does not name one. */
static uint8_t mesh_app_probe_forced(void) {
    const char *protocol = inkwell_env_get("PROTOCOL");
    if (protocol == NULL) {
        return 0U;
    }
    if (strcmp(protocol, "meshcore") == 0) {
        return MESH_APP_PROBE_MESHCORE;
    }
    if (strcmp(protocol, "meshtastic") == 0) {
        return MESH_APP_PROBE_MESHTASTIC;
    }
    return 0U;
}

static struct mesh_app_probe_port *mesh_app_probe_port(struct mesh_app_probe *probe,
                                                       const char *identifier) {
    for (size_t i = 0; i < MESH_APP_PROBE_PORTS; ++i) {
        if (probe->ports[i].identifier[0] != '\0' &&
            strcmp(probe->ports[i].identifier, identifier) == 0) {
            return &probe->ports[i];
        }
    }
    return NULL;
}

/* What a link answered in - or 0 with a mute for one that answered neither. Oldest out. */
static void mesh_app_probe_remember(struct mesh_app_probe *probe, const char *identifier,
                                    uint8_t answered, uint64_t mute_until_ms) {
    struct mesh_app_probe_port *port = mesh_app_probe_port(probe, identifier);
    if (port == NULL) {
        port = &probe->ports[probe->next_port];
        probe->next_port = (probe->next_port + 1U) % MESH_APP_PROBE_PORTS;
        snprintf(port->identifier, sizeof port->identifier, "%s", identifier);
    }
    port->answered = answered;
    port->mute_until_ms = mute_until_ms;
}

static const char *mesh_app_probe_name(uint8_t protocol) {
    return protocol == MESH_APP_PROBE_MESHCORE ? "MeshCore" : "Meshtastic";
}

static int mesh_app_probe_connect(struct mesh_app *app) {
    struct mesh_transport *transport = mesh_app_probe_transport(app->probe.kind);
    return app->probe.kind == (uint8_t)MESH_UI_DEVICE_TCP
               ? mesh_tcp_transport_connect(transport, app->probe.identifier)
               : mesh_serial_transport_connect(transport, app->probe.identifier);
}

static void mesh_app_probe_disconnect(struct mesh_app *app) {
    struct mesh_transport *transport = mesh_app_probe_transport(app->probe.kind);
    if (app->probe.kind == (uint8_t)MESH_UI_DEVICE_TCP) {
        (void)mesh_tcp_transport_disconnect(transport);
    } else {
        (void)mesh_serial_transport_disconnect(transport);
    }
}

/*
 * The name a link is remembered under. A USB port is connected by its tty or by its sysfs id
 * and the first becomes the second's once the driver binds, so a port is remembered by the id,
 * which it has from the scan on. A host is its own name.
 */
static const char *mesh_app_probe_key(uint8_t kind, const char *identifier) {
    if (kind == (uint8_t)MESH_UI_DEVICE_TCP || identifier == NULL) {
        return identifier;
    }
    size_t count = 0U;
    const struct inkwell_serial_port_info *ports =
        mesh_serial_transport_devices(mesh_serial_transport(), &count);
    for (size_t i = 0; ports != NULL && i < count; ++i) {
        if (ports[i].id[0] != '\0' &&
            (strcmp(ports[i].id, identifier) == 0 || strcmp(ports[i].path, identifier) == 0)) {
            return ports[i].id;
        }
    }
    return identifier;
}

void mesh_app_probe_begin(struct mesh_app *app, uint8_t kind, const char *identifier,
                          uint64_t now_ms) {
    struct mesh_app_probe *probe = &app->probe;
    probe->identifier[0] = '\0';

    const uint8_t forced = mesh_app_probe_forced();
    if (forced != 0U || identifier == NULL || identifier[0] == '\0') {
        mesh_app_bind_protocol(app, forced == MESH_APP_PROBE_MESHCORE);
        return;
    }

    /* A connect is a fresh question, so a mute on this link is lifted by it: the user pressing
       the row, or auto-connect coming back once the mute ran out. */
    const char *key = mesh_app_probe_key(kind, identifier);
    struct mesh_app_probe_port *port = mesh_app_probe_port(probe, key);
    const uint8_t first = (port != NULL && port->answered == MESH_APP_PROBE_MESHCORE)
                              ? MESH_APP_PROBE_MESHCORE
                              : MESH_APP_PROBE_MESHTASTIC;
    if (port != NULL) {
        port->mute_until_ms = 0U;
    }

    snprintf(probe->identifier, sizeof probe->identifier, "%s", identifier);
    snprintf(probe->key, sizeof probe->key, "%s", key);
    probe->kind = kind;
    probe->tried = first;
    probe->deadline_ms = now_ms + MESH_APP_PROBE_WINDOW_MS;
    mesh_app_bind_protocol(app, first == MESH_APP_PROBE_MESHCORE);
}

bool mesh_app_probe_muted(const struct mesh_app *app, const char *key, uint64_t now_ms) {
    if (app == NULL || key == NULL) {
        return false;
    }
    for (size_t i = 0; i < MESH_APP_PROBE_PORTS; ++i) {
        const struct mesh_app_probe_port *port = &app->probe.ports[i];
        if (port->identifier[0] != '\0' && strcmp(port->identifier, key) == 0) {
            return port->answered == 0U && now_ms < port->mute_until_ms;
        }
    }
    return false;
}

void mesh_app_probe_tick(struct mesh_app *app, uint64_t now_ms) {
    struct mesh_app_probe *probe = &app->probe;
    if (probe->identifier[0] == '\0') {
        return;
    }
    struct mesh_transport *transport = mesh_app_probe_transport(probe->kind);
    const bool tcp = probe->kind == (uint8_t)MESH_UI_DEVICE_TCP;
    const bool connecting = tcp ? mesh_tcp_transport_is_connecting(transport)
                                : mesh_serial_transport_is_connecting(transport);
    const bool up = tcp ? mesh_tcp_transport_connected_target(transport) != NULL
                        : mesh_serial_transport_connected_port(transport) != NULL;

    /* The link went away some other way - unplugged, refused, another radio chosen. That is
       no answer about the protocol, so the question is dropped rather than decided. */
    if (!up && !connecting) {
        probe->identifier[0] = '\0';
        return;
    }
    /* The window is the radio's to answer in, so a link still coming up does not spend it. */
    if (!up) {
        probe->deadline_ms = now_ms + MESH_APP_PROBE_WINDOW_MS;
        return;
    }

    const size_t frames = tcp ? mesh_tcp_transport_stats(transport).frames_received
                              : mesh_serial_transport_stats(transport).frames_received;
    const uint8_t bound = app->meshcore_bound ? MESH_APP_PROBE_MESHCORE : MESH_APP_PROBE_MESHTASTIC;
    if (frames > 0U) {
        inkwell_log_info("app", "%s answered in %s", probe->identifier, mesh_app_probe_name(bound));
        mesh_app_probe_remember(probe, probe->key, bound, 0U);
        probe->identifier[0] = '\0';
        return;
    }
    if (now_ms < probe->deadline_ms) {
        return;
    }

    const uint8_t untried =
        (uint8_t)((MESH_APP_PROBE_MESHTASTIC | MESH_APP_PROBE_MESHCORE) & ~(unsigned)probe->tried);
    if (untried != 0U) {
        const uint8_t next = (untried & MESH_APP_PROBE_MESHTASTIC) != 0U ? MESH_APP_PROBE_MESHTASTIC
                                                                         : MESH_APP_PROBE_MESHCORE;
        inkwell_log_info("app", "%s sent no %s frame in %u ms; asking in %s", probe->identifier,
                         mesh_app_probe_name(bound), MESH_APP_PROBE_WINDOW_MS,
                         mesh_app_probe_name(next));
        probe->tried |= next;
        probe->deadline_ms = now_ms + MESH_APP_PROBE_WINDOW_MS;
        mesh_app_probe_disconnect(app);
        mesh_app_bind_protocol(app, next == MESH_APP_PROBE_MESHCORE);
        const int result = mesh_app_probe_connect(app);
        if (result < 0 && result != -EALREADY && result != -EINPROGRESS) {
            inkwell_log_warn("app", "Reopening %s failed (%d)", probe->identifier, result);
            probe->identifier[0] = '\0';
        }
        return;
    }

    inkwell_log_warn("app", "%s answered neither Meshtastic nor MeshCore; passing it over for %u s",
                     probe->identifier, MESH_APP_PROBE_MUTE_MS / 1000U);
    mesh_app_probe_remember(probe, probe->key, 0U, now_ms + MESH_APP_PROBE_MUTE_MS);
    char where[MESH_APP_PROBE_ID_MAX];
    snprintf(where, sizeof where, "%s", probe->identifier);
    probe->identifier[0] = '\0';
    mesh_app_probe_disconnect(app);
    if (app->ui_report_link_error && app->config.run_mode == MESH_APP_RUN_FOREGROUND) {
        char toast[MESH_TRANSPORT_ERROR_MAX];
        (void)inkcell_str_format(toast, sizeof toast,
                                 tcp ? MESH_STR_LINK_TCP_NO_ANSWER : MESH_STR_LINK_USB_NO_ANSWER,
                                 where);
        mesh_ui_store_set_toast(&app->ui_store, now_ms, toast);
        app->ui_report_link_error = false;
    }
}
