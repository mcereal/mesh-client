#define _POSIX_C_SOURCE 200809L

/*
 * The process: one event loop, two transports, one link at a time.
 *
 * What is left here after the split is the part that owns lifetime rather than content -
 * bringing the loop, the transports and the UI backend up, routing a connect to the transport
 * that owns the row, deciding on its own which radio to reach for, and tearing it all down.
 * The three neighbouring files hang off the seams in app_internal.h.
 */

#include "inkwell/base/env.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"

#include "app_internal.h"

#include "inkwell/net/fetch.h"
#include "inkwell/runtime/crash.h"
#include "mesh/core/ca_roots.h"
#include "mesh/core/version.h"
#include "mesh/i18n/strings.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/serial.h"
#include "mesh/transport/tcp.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/fb.h"
#include "mesh/ui/backends/stub.h"
#include "mesh/ui/preferences.h"
#include "mesh/ui/route.h"
#include "mesh/utils/crash.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- link routing --------------------------------------------------------------------- */

/*
 * Three links, one session, one radio at a time. The Devices tab lists BLE advertisers and USB
 * ports together, so a connect has to be routed to the transport that owns the row, and taking
 * one link up drops the others.
 *
 * The network link has no row yet - it has no discovery to build one from, and the address it
 * would name has nowhere to be typed on a handheld - so it is reached from configuration rather
 * than from a press. Everything below still has to know about it: a link nothing here counts as
 * connected is one auto-connect will happily talk over.
 */

/*
 * The disconnect chain's twin, and it gets its arms stated for the same reason: each of these
 * transports casts `transport->state` to its own struct, so a kind routed to the wrong one is
 * not a connect that fails, it is a read of one transport's state through another's type. An
 * "everything else" arm means Bluetooth, and a kind that is not Bluetooth falls into it.
 *
 * The network arm was unreachable when it was written - a row is connectable only while it is
 * *not* connected, and the one row a network link had existed only while it was - and it was
 * stated anyway rather than left as a trap for whatever came next. What came next is the Devices
 * tab's own network row, whose A raises exactly this kind.
 */
static struct mesh_transport *mesh_app_transport_for_kind(uint8_t kind) {
    switch ((enum mesh_ui_device_kind)kind) {
    case MESH_UI_DEVICE_SERIAL:
        return mesh_serial_transport();
    case MESH_UI_DEVICE_TCP:
        return mesh_tcp_transport();
    case MESH_UI_DEVICE_BLE:
        break;
    }
    return mesh_ble_transport();
}

struct mesh_transport *mesh_app_active_transport(void) {
    struct mesh_transport *serial = mesh_serial_transport();
    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_transport *tcp = mesh_tcp_transport();
    if (serial != NULL && mesh_serial_transport_connected_port(serial) != NULL) {
        return serial;
    }
    if (tcp != NULL && mesh_tcp_transport_connected_target(tcp) != NULL) {
        return tcp;
    }
    if (ble != NULL && mesh_ble_transport_connected_address(ble) != NULL) {
        return ble;
    }
    if (serial != NULL && mesh_serial_transport_is_connecting(serial)) {
        return serial;
    }
    if (tcp != NULL && mesh_tcp_transport_is_connecting(tcp)) {
        return tcp;
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
    const char *target = mesh_tcp_transport_connected_target(mesh_tcp_transport());
    if (target != NULL && target[0] != '\0') {
        return target;
    }
    const char *address = mesh_ble_transport_connected_address(mesh_ble_transport());
    return (address != NULL && address[0] != '\0') ? address : NULL;
}

bool mesh_app_link_connecting(void) {
    /* Pairing counts: it is the first half of a connect the user asked for, and auto-connect
       taking the serial link up underneath it would leave two transports on one session. */
    return mesh_ble_transport_is_connecting(mesh_ble_transport()) ||
           mesh_ble_transport_is_pairing(mesh_ble_transport()) ||
           mesh_serial_transport_is_connecting(mesh_serial_transport()) ||
           mesh_tcp_transport_is_connecting(mesh_tcp_transport());
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
    struct mesh_transport *tcp = mesh_tcp_transport();
    if (tcp != keep && (mesh_tcp_transport_connected_target(tcp) != NULL ||
                        mesh_tcp_transport_is_connecting(tcp))) {
        mesh_tcp_transport_disconnect(tcp);
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

    /* A third preference for the same reason the first two are apart: a host written down is
       not a radio to look for over the air, and storing it under the BLE preference would send
       auto-connect hunting for an advertisement no address can ever match. */
    if (kind == (uint8_t)MESH_UI_DEVICE_TCP) {
        if (mesh_tcp_transport_connected_target(transport) != NULL ||
            mesh_tcp_transport_is_connecting(transport)) {
            mesh_tcp_transport_disconnect(transport);
        }
        const int result = mesh_tcp_transport_connect(transport, identifier);
        /*
         * What the transport *adopted*, not what it was handed, and asked rather than inferred
         * from the return code.
         *
         * The link takes a target only once it has parsed it and got a socket, so several
         * refusals leave `configured` behind: a typo or a name (-EINVAL), the transport turned
         * off by configuration (-ENODEV), a link already coming up (-EBUSY), no descriptors
         * (-EMFILE). Copying the identifier through any of those leaves this preference naming
         * a host the link is not reaching for - invisible now, because the Devices row and
         * auto-connect both read the transport, and then loaded on the next launch as the host
         * to retry. Enumerating the codes was the first cut of this and it missed three of
         * them; the transport is the authority on which host it is pointed at, so it is asked.
         *
         * A connect that fails *after* adoption keeps the address, which is the case worth
         * keeping: the radio is off, or the Brick is on the wrong WiFi, and it is still the
         * address somebody wrote down.
         */
        const char *adopted = mesh_tcp_transport_configured_target(transport);
        if (adopted != NULL) {
            snprintf(app->config.preferred_tcp_host, sizeof app->config.preferred_tcp_host, "%s",
                     adopted);
        }
        return result;
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
void mesh_app_on_ui_key(void *userdata, enum inkcell_key key) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL) {
        return;
    }
    mesh_ui_controller_handle_key(&app->ui_controller, key);
}

/* A visible action hint names a command, even though inkcell can only name the generic keycap it
   drew. Resolve that binding against the last frame rather than treating the click as hardware. */
void mesh_app_on_ui_action_key(void *userdata, enum inkcell_key key) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL) {
        return;
    }
    mesh_ui_controller_handle_action_key(&app->ui_controller, key);
}

static void mesh_app_on_ui_shortcut(void *userdata, char letter) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app != NULL) {
        mesh_ui_controller_handle_shortcut(&app->ui_controller, letter);
    }
}

static bool mesh_app_ui_text_active(void *userdata) {
    const struct mesh_app *app = (const struct mesh_app *)userdata;
    if (app == NULL) {
        return false;
    }
    struct mesh_ui_route route;
    mesh_ui_route_of(&app->ui_store.nav, &route);
    return route.level == MESH_UI_ROUTE_KEYBOARD;
}

static void mesh_app_on_ui_text(void *userdata, const char *text) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app != NULL) {
        mesh_ui_controller_handle_text(&app->ui_controller, text);
    }
}

/* A click the window could not answer as a key: a tab, a row, a dialog's answer. Where in the
   box it landed says nothing a press does not. */
void mesh_app_on_ui_click(void *userdata, uint32_t target, int x, int y) {
    (void)x;
    (void)y;
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL) {
        return;
    }
    mesh_ui_controller_handle_click(&app->ui_controller, target);
}

/* A right-click: where it landed is where the menu hangs, so here the point does matter. */
void mesh_app_on_ui_context(void *userdata, uint32_t target, int x, int y) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL) {
        return;
    }
    mesh_ui_controller_handle_context(&app->ui_controller, target, x, y);
}

static void mesh_app_select_cli(struct mesh_app *app, const struct inkcell_backend **backend,
                                void **userdata) {
    if (backend != NULL) {
        *backend = mesh_ui_backend_cli();
    }
    if (userdata != NULL) {
        *userdata = &app->ui_cli_context;
    }
}

static void mesh_app_select_stub(const struct inkcell_backend **backend, void **userdata) {
    if (backend != NULL) {
        *backend = mesh_ui_backend_stub();
    }
    if (userdata != NULL) {
        *userdata = NULL;
    }
}

/*
 * The loop inkcell's input layer reads on.
 *
 * inkcell does not own an event loop - this process has one already, and a UI library that
 * brought a second would be asking every application to run two - so the three things reading a
 * device needs from a loop are a vtable it is handed. These are that vtable over inkwell_loop,
 * and they are thin on purpose: anything cleverer here would be the loop's policy written twice.
 */
static int mesh_app_ui_add_fd(void *ctx, int fd,
                              int (*callback)(int fd, uint32_t events, void *userdata),
                              void *userdata) {
    return inkwell_loop_add_fd((struct inkwell_loop *)ctx, fd, INKWELL_LOOP_IN, callback, userdata);
}

static void mesh_app_ui_remove_fd(void *ctx, int fd) {
    inkwell_loop_remove_fd((struct inkwell_loop *)ctx, fd);
}

/* A quit key. What stopping *is* belongs to whoever owns the loop, which is why inkcell asks
   rather than doing it. */
static void mesh_app_ui_request_stop(void *ctx) {
    inkwell_loop_request_stop((struct inkwell_loop *)ctx);
}

/* The window asking for a frame it cannot draw without the snapshot. */
static void mesh_app_ui_request_frame(void *userdata) {
    mesh_ui_controller_request_frame(&((struct mesh_app *)userdata)->ui_controller);
}

/*
 * The window backend, where there is a window to open.
 *
 * Off unless it is asked for by name. On the device the framebuffer is the UI - it is what the
 * pak runs and the one that has been measured - and this is the backend for a development
 * host, where until now the only way to see a screen was to render it to a GIF.
 *
 * Unlike the framebuffer's, this backend reads its own buttons: an SDL window's presses come
 * off the same queue as its resize and its close box, so the thing that owns the window owns
 * them. See `ui_backend_reads_input` for what that means for the evdev reader.
 */
static bool mesh_app_select_sdl(struct mesh_app *app, const struct inkcell_backend **backend,
                                void **userdata) {
    if (!inkcell_backend_sdl_is_available()) {
        return false;
    }

    if (backend != NULL) {
        *backend = inkcell_backend_sdl();
    }
    if (userdata != NULL) {
        app->ui_sdl_context = (struct inkcell_backend_sdl_context){
            .app = fb_app_vtable(),
            /* The same three calls over inkwell_loop the evdev reader is handed, because SDL
               has no descriptor of its own to watch and inkcell puts a timerfd here instead -
               see inkcell/ui/sdl.h. */
            .host = {.ctx = &app->loop,
                     .add_fd = mesh_app_ui_add_fd,
                     .remove_fd = mesh_app_ui_remove_fd,
                     .request_stop = mesh_app_ui_request_stop},
            .on_key = mesh_app_on_ui_key,
            .on_action_key = mesh_app_on_ui_action_key,
            .on_shortcut = mesh_app_on_ui_shortcut,
            .text_input_active = mesh_app_ui_text_active,
            .on_text_input = mesh_app_on_ui_text,
            .key_userdata = app,
            .on_click = mesh_app_on_ui_click,
            .on_context = mesh_app_on_ui_context,
            .click_userdata = app,
            /* What the window manager puts on the title bar: the product's name, which is
               not a word anybody translates - the same fact mesh_crash_install() states
               about a crash report. */
            .title = "MeshClient",
            /* The frame opens with the tab strip, so on a Mac the window's buttons can sit in
               it - see inkcell/ui/sdl.h. */
            .unified_titlebar = true,
            .request_frame = mesh_app_ui_request_frame,
            .frame_userdata = app,
        };
        *userdata = &app->ui_sdl_context;
    }
    app->ui_backend_reads_input = true;
    return true;
}

/*
 * The device's frame, drawn into memory and shown to nobody - for a host with no panel and no
 * display, where something drives the client through the control socket and looks at what it
 * drew. Always opens. Reads no buttons of its own, so the evdev reader stays on where there is
 * one.
 */
static void mesh_app_select_headless(struct mesh_app *app, const struct inkcell_backend **backend,
                                     void **userdata) {
    if (backend != NULL) {
        *backend = inkcell_backend_headless();
    }
    if (userdata != NULL) {
        app->ui_headless_context = (struct inkcell_backend_headless_context){
            .app = fb_app_vtable(),
        };
        *userdata = &app->ui_headless_context;
    }
}

static bool mesh_app_select_fb(struct mesh_app *app, const struct inkcell_backend **backend,
                               void **userdata) {
    if (!inkcell_backend_fb_is_available()) {
        return false;
    }

    if (backend != NULL) {
        *backend = inkcell_backend_fb();
    }
    if (userdata != NULL) {
        /* What draws the frame, rather than a loop: inkcell owns the panel and calls up into
           src/ui/backends/fb_app.c once a frame. */
        app->ui_fb_context.app = fb_app_vtable();
        *userdata = &app->ui_fb_context;
    }
    return true;
}

static const struct inkcell_backend *mesh_app_select_backend(struct mesh_app *app,
                                                             void **userdata) {
    if (userdata != NULL) {
        *userdata = NULL;
    }

    const char *requested = getenv("MESHCLIENT_UI_BACKEND");
    if (requested != NULL && requested[0] == '\0') {
        requested = NULL;
    }

    const struct inkcell_backend *backend = NULL;
    void *backend_userdata = NULL;

    app->ui_backend_reads_input = false;

    /* "cli", "stub", "headless" and "sdl" are asked for explicitly; everything else - including no
       request at all - resolves to the framebuffer, which is what the pak runs, and falls back to
       the CLI backend only where there is no /dev/fb0 to draw on (a container, or a dev host).
       "sdl" is not in the fallback chain on purpose: a window is a thing somebody asks for, and
       a client that silently opened one because the panel was missing would be a surprise on
       any host with a display. */
    if (requested != NULL && strcasecmp(requested, "cli") == 0) {
        mesh_app_select_cli(app, &backend, &backend_userdata);
    } else if (requested != NULL && strcasecmp(requested, "stub") == 0) {
        mesh_app_select_stub(&backend, &backend_userdata);
    } else if (requested != NULL && strcasecmp(requested, "headless") == 0) {
        mesh_app_select_headless(app, &backend, &backend_userdata);
    } else if (requested != NULL && strcasecmp(requested, "sdl") == 0) {
        if (!mesh_app_select_sdl(app, &backend, &backend_userdata)) {
            inkwell_log_warn("ui", "No SDL window backend in this build or on this host; "
                                   "using the default");
            if (!mesh_app_select_fb(app, &backend, &backend_userdata)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        }
    } else {
        if (requested != NULL && strcasecmp(requested, "fb") != 0 &&
            strcasecmp(requested, "auto") != 0) {
            inkwell_log_warn("ui", "Unknown UI backend '%s'; using the default", requested);
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

void mesh_app_note_connected_device(struct mesh_app *app, const char *identifier, uint8_t kind) {
    if (app == NULL || identifier == NULL || identifier[0] == '\0') {
        return;
    }
    if (mesh_ui_preferences_note_device(&app->ui_preferences, identifier, kind)) {
        app->ui_preferences_dirty = true;
    }
    /* --preferred-device pinned whatever the launch asked for, and this overwrites it on
       purpose: the flag says which node to reach for, and the node you are on now is a later
       and better answer to that than the one you named before you left the house. */
    char *slot = kind == (uint8_t)MESH_UI_DEVICE_SERIAL ? app->config.preferred_serial_device
                                                        : app->config.preferred_ble_device;
    const size_t slot_len = kind == (uint8_t)MESH_UI_DEVICE_SERIAL
                                ? sizeof app->config.preferred_serial_device
                                : sizeof app->config.preferred_ble_device;
    if (strcmp(slot, identifier) != 0) {
        snprintf(slot, slot_len, "%s", identifier);
        /* A different node is a different wait. The grace period is how long *this* node gets
           to show up, so a switch restarts it: without this, a radio chosen an hour into a
           session would be handed a window that expired at launch, and the first drop after it
           would go straight back to the node it replaced. */
        app->autoconnect_started_ms = 0U;
        app->autoconnect_waiting_logged = false;
    }
}

#define MESH_APP_AUTOCONNECT_RETRY_MS 2000U
/*
 * How long the network arm waits after an attempt that produced no link.
 *
 * Longer than the retry above because a network attempt costs the whole connect deadline before
 * it can fail, and what has to fit in the gap is the other two arms: this is what lets a client
 * with a stale address still find the node in its pocket.
 */
#define MESH_APP_AUTOCONNECT_TCP_RETRY_MS 30000U
#define MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS 60000U
/* How long a saved preferred node gets to show up in discovery before a stranger is used. */
#define MESH_APP_AUTOCONNECT_PREFERRED_GRACE_MS 30000U
/*
 * And how long it gets when another radio of ours is already advertising.
 *
 * The long grace is the price of not settling for a node that is not yours, and it is worth
 * paying when the alternative is a stranger. It is not worth paying when the alternative is the
 * second radio you own, sitting in your hand: walking out of the house with it means the saved
 * node is at home and is never going to answer, and half a minute of "connecting..." is the
 * whole of what that wait buys. Long enough that a node which is merely slow to advertise still
 * wins its slot, short enough that being wrong about it costs a few seconds.
 *
 * Only at launch, though. After a link to the *preferred* node drops, the long grace applies
 * whoever else is in range: the usual reason is a settings write, which reboots the radio, and
 * a Heltec V4 saving its owner was not advertising again five seconds later. With the short wait
 * there, the second radio on the desk took the slot and every screen after it was that radio's,
 * including the save the user thought they were making to the first one. The cost is a radio
 * that really has gone - carried out of range mid-session - holding the slot for half a minute.
 */
#define MESH_APP_AUTOCONNECT_KNOWN_GRACE_MS 5000U

/*
 * Whether the link that is up is the preferred node's, over the air - the only one whose
 * ending earns the long wait above.
 *
 * A cable or a network host says nothing about whether the radio we reach for over Bluetooth is
 * rebooting, so neither arms it; nor does a second radio of ours we happen to be on, because
 * that one we can go back to in five seconds. Asked on every turn the link is up rather than
 * once, so the answer is the current link's when it ends.
 *
 * Against the address, because that is what the preference holds while a link is up: a name
 * from --preferred-device or the environment is rewritten to the address actually reached by
 * mesh_app_note_connected_device(), which the publish turn calls for every BLE link.
 */
static bool mesh_app_preferred_ble_link_up(const struct mesh_app *app, struct mesh_transport *ble) {
    if (app->config.preferred_ble_device[0] == '\0') {
        return false;
    }
    const char *const address = mesh_ble_transport_connected_address(ble);
    return address != NULL && strcasecmp(address, app->config.preferred_ble_device) == 0;
}

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
    app->autoconnect_retry_at_ms = inkwell_time_monotonic_ms() + delay;
    return delay;
}

void mesh_app_autoconnect(struct mesh_app *app) {
    if (app == NULL || app->autoconnect_disabled || app->autoconnect_held ||
        app->config.run_mode != MESH_APP_RUN_FOREGROUND) {
        return;
    }
    /* Derived rather than held: a download that fails lifts this by failing. Pairing it with a
       flag would leave a radio unreachable after a failed install until something remembered to
       clear it. */
    if (mesh_updater_holds_the_radio(&app->updater)) {
        return;
    }
    /*
     * A radio firmware install, at either end of what it does with a bus.
     *
     * Two questions rather than one, and the difference is what makes the BLE path work at all.
     * The **antenna** is held while the image comes down - Wi-Fi and Bluetooth are one part
     * here - and released the moment it lands, which is precisely so auto-connect can bring the
     * radio back to be armed. The **radio** is held from the admin verb onwards, because what
     * is on the other end from there is a bootloader's CDC or a loader's GATT and connecting to
     * either would be this client binding the thing it is trying to write to.
     */
    if (mesh_firmware_update_holds_the_antenna(&app->firmware_update) ||
        mesh_firmware_update_holds_the_radio(&app->firmware_update)) {
        return;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    const bool link_up = (mesh_app_connected_identifier() != NULL);
    if (link_up) {
        /* An established link is the only proof an attempt worked, so it is the only thing that
           clears the backoff. It also re-arms the grace period for whatever comes after this
           link, rather than leaving it expired for the life of the process: a settings write
           reboots the radio, and a preferred node that is a few seconds from advertising again
           must not lose its slot to the second radio on the desk. */
        app->autoconnect_failures = 0U;
        app->autoconnect_started_ms = 0U;
        app->autoconnect_tcp_retry_at_ms = 0U;
        app->autoconnect_waiting_logged = false;
        app->autoconnect_after_link = mesh_app_preferred_ble_link_up(app, ble);
    }
    if (ble == NULL || link_up || mesh_app_link_connecting()) {
        return;
    }

    uint64_t now = inkwell_time_monotonic_ms();
    if (now < app->autoconnect_retry_at_ms) {
        return;
    }

    /*
     * A plugged-in node wins over anything on the air: it needs no pairing, has no range to
     * lose, and is almost certainly why the cable is there. BLE keeps its own policy below for
     * when nothing is plugged in.
     */
    struct inkwell_serial_port_info all_ports[MESH_SERIAL_MAX_DEVICES];
    struct mesh_transport *serial = mesh_serial_transport();
    const size_t all_port_count =
        mesh_serial_transport_get_devices(serial, all_ports, MESH_SERIAL_MAX_DEVICES);

    /*
     * A node in its bootloader is still a row in the Devices tab - a refusal is a row, not
     * silence - but it is not a candidate here. The transport refuses it anyway, so this is not
     * what makes the client correct; it is what stops auto-connect attempting the same refusal
     * on every retry and logging a failure that names a fault the user does not have. With a
     * bootloader as the only thing plugged in, the right behaviour is to fall through to
     * Bluetooth exactly as an empty port list does.
     */
    struct inkwell_serial_port_info ports[MESH_SERIAL_MAX_DEVICES];
    size_t port_count = 0U;
    for (size_t i = 0; i < all_port_count; ++i) {
        if (mesh_serial_device_is_radio(&all_ports[i])) {
            ports[port_count++] = all_ports[i];
        }
    }

    if (port_count > 0U) {
        const struct inkwell_serial_port_info *port = &ports[0];
        const char *preferred_port = app->config.preferred_serial_device;
        bool port_chosen = false;
        if (preferred_port[0] != '\0') {
            for (size_t i = 0; i < port_count; ++i) {
                if (strcmp(ports[i].id, preferred_port) == 0 ||
                    (ports[i].path[0] != '\0' && strcmp(ports[i].path, preferred_port) == 0)) {
                    port = &ports[i];
                    port_chosen = true;
                    break;
                }
            }
        }
        /* Two cables is rarer than two radios, but the question is the same one: with the
           preferred port absent, the port we used most recently beats whichever the kernel
           happened to enumerate first. There is no grace period here - a plugged-in node is
           already present, so nothing is going to show up by waiting. */
        if (!port_chosen) {
            int best_rank = -1;
            for (size_t i = 0; i < port_count; ++i) {
                const char *identifier = ports[i].path[0] != '\0' ? ports[i].path : ports[i].id;
                const int rank = mesh_ui_preferences_device_rank(&app->ui_preferences, identifier,
                                                                 (uint8_t)MESH_UI_DEVICE_SERIAL);
                if (rank >= 0 && (best_rank < 0 || rank < best_rank)) {
                    best_rank = rank;
                    port = &ports[i];
                }
            }
        }
        const char *identifier = port->path[0] != '\0' ? port->path : port->id;
        const int serial_result =
            mesh_app_link_connect(app, identifier, (uint8_t)MESH_UI_DEVICE_SERIAL);
        if (serial_result == 0 || serial_result == -EALREADY || serial_result == -EINPROGRESS) {
            if (serial_result == 0) {
                inkwell_log_info("app", "Auto-connecting to %s over USB (%s)", port->name,
                                 identifier);
            }
            /* Not a success yet: the handshake still has to go out. The counter stays where it
               is until a link is actually up. */
            app->autoconnect_retry_at_ms = now + MESH_APP_AUTOCONNECT_RETRY_MS;
            return;
        }
        inkwell_log_warn("app", "Auto-connect to %s over USB failed (%d); trying the network",
                         identifier, serial_result);
    }

    /*
     * Then a configured network host. Like a cable it needs no pairing and has no range to lose,
     * and unlike anything on the air it is a place somebody deliberately wrote down - so it
     * outranks a scan. It is only ever tried when a host was configured, which is empty by
     * default: a client that has never set one behaves exactly as it did before this link.
     */
    struct mesh_transport *tcp = mesh_tcp_transport();
    const char *tcp_target = mesh_tcp_transport_configured_target(tcp);
    if (tcp_target != NULL && now >= app->autoconnect_tcp_retry_at_ms) {
        /* Stamped before the attempt, not after it: most of the ways this fails do so on the
           connect deadline, long after the call returned 0 and this function went home. */
        app->autoconnect_tcp_retry_at_ms = now + MESH_APP_AUTOCONNECT_TCP_RETRY_MS;
        mesh_app_release_other_link(tcp);
        const int tcp_result = mesh_tcp_transport_connect(tcp, tcp_target);
        if (tcp_result == 0) {
            inkwell_log_info("app", "Auto-connecting to %s over the network", tcp_target);
            /* Not a success yet: the connect has not completed and the handshake has not gone
               out. The counter stays where it is until a link is actually up. */
            app->autoconnect_retry_at_ms = now + MESH_APP_AUTOCONNECT_RETRY_MS;
            return;
        }
        inkwell_log_warn("app", "Auto-connect to %s over the network failed (%d); trying Bluetooth",
                         tcp_target, tcp_result);
    }

    struct inkwell_ble_device devices[MESH_UI_MAX_DEVICES];
    size_t device_count = mesh_ble_transport_get_devices(ble, devices, MESH_UI_MAX_DEVICES);

    /*
     * Only a node that answered this scan is a candidate.
     *
     * The enumeration behind that list is a walk of every device object BlueZ holds, and a bond
     * outlives the radio being in the room - so a node left at home is in it all day, with the
     * saved address and the saved name and Paired set. Auto-connect used to take that as its
     * target and spend the whole session timing out against a radio in another building while
     * the one in your pocket advertised into an empty list. See inkwell_ble_device.in_range.
     */
    size_t in_range[MESH_UI_MAX_DEVICES];
    size_t in_range_count = 0U;
    for (size_t i = 0; i < device_count; ++i) {
        if (devices[i].in_range) {
            in_range[in_range_count++] = i;
        }
    }
    if (in_range_count == 0U) {
        return; /* nothing in earshot yet; discovery keeps running */
    }

    /* The grace period below is a scanning window, so it starts at the first node actually
       heard rather than at app start: BlueZ can arrive long after we do (the first launch after
       the Brick wakes), and a window that expired while nothing was scanning would hand the
       preferred node's slot to whatever answered first. */
    if (app->autoconnect_started_ms == 0U) {
        app->autoconnect_started_ms = now;
    }

    const struct inkwell_ble_device *target = NULL;
    const char *preferred = app->config.preferred_ble_device;
    if (preferred[0] != '\0') {
        for (size_t i = 0; i < in_range_count; ++i) {
            const struct inkwell_ble_device *device = &devices[in_range[i]];
            if (strcasecmp(device->address, preferred) == 0 ||
                strcasecmp(device->name, preferred) == 0) {
                target = device;
                break;
            }
        }
    }

    /* Failing that, the radio of ours that is in earshot and was used most recently. Rank order
       is the whole point: with three of your own nodes on the table you get the one you were
       using, and the loudest advertiser only decides between nodes you have never connected
       to. */
    if (target == NULL) {
        const struct inkwell_ble_device *known = NULL;
        int best_rank = -1;
        for (size_t i = 0; i < in_range_count; ++i) {
            const struct inkwell_ble_device *device = &devices[in_range[i]];
            const int rank = mesh_ui_preferences_device_rank(&app->ui_preferences, device->address,
                                                             (uint8_t)MESH_UI_DEVICE_BLE);
            if (rank >= 0 && (best_rank < 0 || rank < best_rank)) {
                best_rank = rank;
                known = device;
            }
        }

        if (preferred[0] != '\0') {
            const uint64_t grace = known != NULL && !app->autoconnect_after_link
                                       ? MESH_APP_AUTOCONNECT_KNOWN_GRACE_MS
                                       : MESH_APP_AUTOCONNECT_PREFERRED_GRACE_MS;
            if (now - app->autoconnect_started_ms < grace) {
                if (!app->autoconnect_waiting_logged) {
                    inkwell_log_info("app", "Preferred device '%s' not in range yet; waiting",
                                     preferred);
                    app->autoconnect_waiting_logged = true;
                }
                app->autoconnect_retry_at_ms = now + 1000U;
                return;
            }
        }
        target = known;
        if (target != NULL) {
            inkwell_log_info("app", "%s; using your most recent node %s (%s, %d dBm)",
                             preferred[0] != '\0' ? "Preferred device not in range"
                                                  : "No preferred device saved",
                             target->name, target->address, (int)target->rssi);
        }
    }

    if (target == NULL) {
        size_t best = in_range[0];
        for (size_t i = 1; i < in_range_count; ++i) {
            if (devices[in_range[i]].rssi > devices[best].rssi) {
                best = in_range[i];
            }
        }
        target = &devices[best];
        inkwell_log_info("app", "%s; using strongest node %s (%s, %d dBm)",
                         preferred[0] != '\0' ? "Preferred device not in range"
                                              : "No node of yours in range",
                         target->name, target->address, (int)target->rssi);
    }

    int result = mesh_ble_transport_connect(ble, target->address);
    if (result == 0 || result == -EALREADY || result == -EINPROGRESS) {
        if (result == 0) {
            inkwell_log_info("app", "Auto-connecting to %s (%s)", target->name, target->address);
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
    inkwell_log_warn("app", "Auto-connect to %s failed (%d); retrying in %llu ms", target->address,
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
        inkwell_log_info("ui", "Link failure shown to the user: %s", link_error);
        mesh_ui_store_set_toast(&app->ui_store, inkwell_time_monotonic_ms(), link_error);
    } else {
        inkwell_log_debug("ui", "Link failure not shown (auto-connect): %s", link_error);
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

    /* main() supplies uninitialized storage. Initialize every owned field, including lazy
       allocations and notification counters, without requiring callers to zero the app.
       Copy the config first because callers may pass &app->config. */
    struct mesh_app_config initial_config = config != NULL ? *config : mesh_app_config_default();
    if (config == NULL) {
        mesh_app_config_apply_env_overrides(&initial_config);
    }
    memset(app, 0, sizeof *app);
    app->config = initial_config;

    /*
     * Before anything reads a knob, asks for a word, or opens a connection.
     *
     * inkcell reads its environment under a prefix the application sets, so the toolkit's knobs
     * and this client's are one namespace - MESHCLIENT_THEME and MESHCLIENT_AUTOCONNECT rather
     * than one of each. The catalog is the same shape of statement: inkcell's fifteen ids and
     * this client's nine hundred are one table, registered once.
     *
     * The last two are the same statement again, made downwards. Which roots a connection is
     * verified against and what a request calls itself are decisions about this product, and the
     * layers that use them have no way to make one: the TLS client knows how to check a chain but
     * not whose, and the fetcher knows how to make a request but not on whose behalf. Both are
     * answered here, once, and neither has a default that would let a build forget.
     */
    inkwell_env_set_prefix("MESHCLIENT");
    mesh_i18n_register();
    inkwell_tls_set_roots(mesh_ca_roots, mesh_ca_root_count);
    char user_agent[64];
    (void)snprintf(user_agent, sizeof user_agent, "meshclient/%s", mesh_version_string());
    inkwell_fetch_set_user_agent(user_agent);

    int result = inkwell_loop_init(&app->loop);
    if (result < 0) {
        inkwell_log_error("app", "Event loop init failed: %d", result);
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
    app->autoconnect_after_link = false;
    app->ui_link_was_connected = false;
    app->ui_report_link_error = false;
    app->autoconnect_disabled = !inkwell_env_bool("AUTOCONNECT", "auto-connect", true);
    if (app->autoconnect_disabled) {
        inkwell_log_info("app", "Auto-connect disabled by MESHCLIENT_AUTOCONNECT");
    }
    app->ui_handshake_cache_dirty = false;
    app->ui_cache_timer_armed = false;
    app->ui_cache_timer_fd = -1;
    app->ui_read_state_revision = 0U;

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
            /*
             * And the network address the Devices tab typed, on the same terms: the flag and
             * the environment variable win, because somebody who passed --tcp-host on this
             * launch means this launch. Before this existed the address could only come from
             * one of those two, which on a handheld means editing launch.sh on the card.
             *
             * Read here rather than by the transport because the transport is configured from
             * `config` a few hundred lines further down, and one source for a setting is what
             * stops the two disagreeing about which host auto-connect is reaching for.
             */
            if (app->config.preferred_tcp_host[0] == '\0') {
                snprintf(app->config.preferred_tcp_host, sizeof app->config.preferred_tcp_host,
                         "%s", app->ui_preferences.network_host);
            }
        }
        /*
         * The crash handler, as soon as there is a directory to write into.
         *
         * Here rather than at the top of main() because the report has to land beside the
         * preferences and the caches - the one directory a user on a handheld can be told to
         * look in - and that path is not known until now. What is lost by waiting is a fault
         * during the few hundred microseconds of argument parsing above, which is code that
         * runs identically on every launch: a crash in it would be the one crash nobody needs
         * a report to reproduce.
         *
         * A failure is logged and otherwise ignored. A client that cannot write crash reports
         * is still a client, and refusing to start over it would turn a diagnostic into an
         * outage.
         */
        char data_dir[sizeof app->ui_preferences_path];
        inkwell_str_copy(data_dir, sizeof data_dir, app->ui_preferences_path);
        char *data_slash = strrchr(data_dir, '/');
        if (data_slash != NULL && data_slash != data_dir) {
            *data_slash = '\0';
            const int crash_result = mesh_crash_install(data_dir);
            if (crash_result < 0) {
                inkwell_log_warn("app", "Crash reports unavailable: %d", crash_result);
            } else if (inkwell_crash_report_waiting()) {
                inkwell_log_warn("app", "A crash report from a previous run is waiting in %s",
                                 data_dir);
            }
        }
        /* inkcell writes the log; the crash reporter wants a copy of each line, and says so
           rather than being called by name from inside the logger. */
        inkwell_log_set_sink(inkwell_crash_log_line);
        inkwell_crash_note(MESH_CRASH_NOTE_VERSION, mesh_version_string());

        int handshake_written =
            snprintf(app->ui_handshake_cache_path, sizeof(app->ui_handshake_cache_path),
                     "%s.handshake", app->ui_preferences_path);
        if (handshake_written < 0 ||
            handshake_written >= (int)sizeof(app->ui_handshake_cache_path)) {
            inkwell_log_warn("app", "Handshake cache path truncated; disabling cache");
            app->ui_handshake_cache_path[0] = '\0';
        }

        /*
         * The transcript, in a directory beside the cache rather than in it.
         *
         * A failure here is logged and otherwise ignored, exactly as the crash reporter's is: a
         * client that cannot keep history is still a client, and the live view comes from the
         * transport ring either way.
         */
        char archive_dir[sizeof app->ui_preferences_path + 16];
        const int archive_written =
            snprintf(archive_dir, sizeof archive_dir, "%s.messages", app->ui_preferences_path);
        if (archive_written > 0 && archive_written < (int)sizeof archive_dir) {
            const int archive_result = mesh_ui_archive_init(&app->ui_archive, archive_dir);
            if (archive_result < 0) {
                inkwell_log_warn("app", "Message archive unavailable: %d", archive_result);
            }
        } else {
            inkwell_log_warn("app", "Message archive path truncated; disabling the transcript");
        }

        /* And the trend log, in a directory of its own beside both. Its failure is the same
           shrug: a client that cannot remember what a node's battery has been doing is still a
           client, and the live trend comes from the history either way. */
        char trends_dir[sizeof app->ui_preferences_path + 16];
        const int trends_written =
            snprintf(trends_dir, sizeof trends_dir, "%s.trends", app->ui_preferences_path);
        if (trends_written > 0 && trends_written < (int)sizeof trends_dir) {
            const int trends_result = mesh_ui_trends_init(&app->ui_trends, trends_dir);
            if (trends_result < 0) {
                inkwell_log_warn("app", "Trend log unavailable: %d", trends_result);
            }
        } else {
            inkwell_log_warn("app", "Trend log path truncated; disabling node trends");
        }
    }

    result = mesh_ui_store_init(&app->ui_store);
    if (result < 0) {
        inkwell_log_error("app", "UI store init failed: %d", result);
        inkwell_loop_shutdown(&app->loop);
        return result;
    }

    if (app->ui_handshake_cache_path[0] != '\0') {
        int handshake_load = mesh_ui_store_load(&app->ui_store, app->ui_handshake_cache_path);
        if (handshake_load < 0 && handshake_load != -ENOENT) {
            inkwell_log_debug("app", "Failed to load handshake cache: %d", handshake_load);
        }
    }

    /* Keep the restored conversation aside: every publish merges it back in, so an empty
       transport log at startup never overwrites it. */
    app->ui_messages_cached = app->ui_store.messages;
    /*
     * And into the archive, for the conversations that have no file yet.
     *
     * This is the upgrade path and it only ever runs once per conversation: a card coming from
     * a build without an archive has 64 messages in the handshake cache and nothing else, and
     * without this their transcript would begin at the moment of the upgrade rather than
     * carrying what the client already knew. A conversation that has a file is untouched.
     */
    const int seeded = mesh_ui_archive_seed(&app->ui_archive, &app->ui_messages_cached);
    if (seeded > 0) {
        inkwell_log_info("app", "Seeded the message archive with %d restored message(s)", seeded);
    }
    /*
     * The radio the trend logs on the card are about, off the cache rather than off a link that
     * has not been made yet.
     *
     * Without it a client relaunched against a *different* radio would read the previous one's
     * trends back for any node number the two meshes happen to share - the exact thing
     * mesh_ui_history_forget() exists to prevent, arriving by the one route the store cannot
     * see, because from the store's side both runs start from nothing.
     */
    (void)mesh_ui_trends_note_radio(&app->ui_trends, app->ui_store.handshake.roster_owner);

    /* Restored read marks are already on disk; only later ones need a save. */
    app->ui_read_state_revision = app->ui_store.read_state.revision;

    void *backend_userdata = NULL;
    const struct inkcell_backend *ui_backend = mesh_app_select_backend(app, &backend_userdata);
    result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store, ui_backend,
                                     backend_userdata, &app->loop);
    /*
     * Selected, and then it would not open.
     *
     * Choosing a backend and opening one are two different moments, and only the first can be
     * answered by asking whether it is available: /dev/fb0 exists or it does not, but a window
     * has a dozen ways to refuse after the library has said yes - no display to reach, a
     * compositor that declined, a texture the driver would not make. The controller treats such
     * a refusal as "this run has no UI", which is right for a headless run and wrong here,
     * because it returns success and the fallback below never sees it.
     *
     * So ask, and if the choice did not open, go down the chain that would have been used had
     * it never been offered. The retry is the default chain rather than mesh_app_select_backend()
     * again: that would read the same environment variable and pick the same backend a second
     * time.
     */
    if (result >= 0 && ui_backend != NULL && !mesh_ui_controller_has_backend(&app->ui_controller)) {
        inkwell_log_warn("ui", "The %s backend would not open; falling back",
                         ui_backend->name != NULL ? ui_backend->name : "selected");
        mesh_ui_controller_shutdown(&app->ui_controller);
        /* ...and whatever it was going to read its own buttons with is gone with it. */
        app->ui_backend_reads_input = false;
        ui_backend = NULL;
        backend_userdata = NULL;
        if (!mesh_app_select_fb(app, &ui_backend, &backend_userdata)) {
            mesh_app_select_cli(app, &ui_backend, &backend_userdata);
        }
        result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store, ui_backend,
                                         backend_userdata, &app->loop);
    }
    if (result < 0) {
        inkwell_log_warn("app", "UI backend init failed (%d); falling back to stub", result);
        result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store,
                                         mesh_ui_backend_stub(), NULL, &app->loop);
    }
    if (result < 0) {
        inkwell_log_error("app", "UI controller init failed: %d", result);
        mesh_ui_store_shutdown(&app->ui_store);
        inkwell_loop_shutdown(&app->loop);
        return result;
    }
    mesh_ui_controller_set_action_handler(&app->ui_controller, mesh_app_on_ui_action, app);

    /*
     * The look, in order of who gets to decide: MESHCLIENT_THEME, then whatever was picked in
     * Settings on an earlier run, then the default. The environment wins because it is the
     * deliberate override - the same order the dev-updates switch below uses - and when it has
     * spoken the About row shows the theme as a fact rather than as a switch.
     */
    /* Resolve the saved language before building any translated UI state. */
    inkcell_i18n_init_with_preference(app->ui_preferences.language);

    app->ui_theme = inkcell_theme_env();
    app->ui_theme_from_env = (app->ui_theme != NULL);
    if (app->ui_theme == NULL) {
        app->ui_theme = inkcell_theme_resolve(app->ui_preferences.theme);
    }

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

    /* The radio's firmware, on its own fetcher because the two are separate presses that must
       be able to fail separately. Never fatal, for the same reason as above. */
    (void)mesh_firmware_init(&app->firmware, &app->loop);
    /* After init, which zeroes the struct. A prefs file written before the setting existed
       reads as stable, so this is a no-op for anyone who has never picked. */
    (void)mesh_firmware_set_channel(
        &app->firmware, (enum mesh_firmware_channel)app->ui_preferences.firmware_channel);

    /* Installing it, on a third fetcher. The check and the install are one press each and
       either can be pressed while the other is in flight - a check that took the download's
       connection out from under it would be this client stopping mid-image because somebody
       asked a question. */
    (void)mesh_firmware_update_init(&app->firmware_update, &app->loop);

    /* The MQTT proxy, which is driven entirely by what the radio asks for. Nothing connects here:
       the decision is re-derived on every loop turn from the radio's own configuration. See
       src/app/app_mqtt.c. */
    mesh_app_mqtt_init(app);

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
                inkwell_log_info("app", "Loaded %d canned replies from %s", loaded, canned_path);
            } else if (loaded != -ENOENT) {
                inkwell_log_warn("app", "Ignoring %s: %d", canned_path, loaded);
            }
        }
    }

    mesh_transport_registry_init(&app->transport_registry);

    result = mesh_transport_registry_register(&app->transport_registry, mesh_ble_transport());
    if (result < 0) {
        inkwell_log_error("app", "Failed to register BLE transport: %d", result);
        mesh_ui_controller_shutdown(&app->ui_controller);
        mesh_ui_store_shutdown(&app->ui_store);
        inkwell_loop_shutdown(&app->loop);
        return result;
    }

    result = mesh_transport_registry_register(&app->transport_registry, mesh_serial_transport());
    if (result < 0) {
        inkwell_log_error("app", "Failed to register serial transport: %d", result);
        mesh_ui_controller_shutdown(&app->ui_controller);
        mesh_ui_store_shutdown(&app->ui_store);
        inkwell_loop_shutdown(&app->loop);
        return result;
    }

    result = mesh_transport_registry_register(&app->transport_registry, mesh_tcp_transport());
    if (result < 0) {
        inkwell_log_error("app", "Failed to register network transport: %d", result);
        mesh_ui_controller_shutdown(&app->ui_controller);
        mesh_ui_store_shutdown(&app->ui_store);
        inkwell_loop_shutdown(&app->loop);
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
    free(app->publish_cache);
    app->publish_cache = NULL;
    inkcell_input_shutdown(&app->ui_input);
    inkwell_signals_shutdown(&app->signals);
    /* Before the loop goes: the updater has an fd registered with it, and a half-finished
       download to clean up. The broker connection goes for the same reason, and goes first so
       the DISCONNECT is written while there is still a session to write it about. */
    mesh_app_mqtt_shutdown(app);
    mesh_updater_shutdown(&app->updater);
    mesh_firmware_shutdown(&app->firmware);
    mesh_firmware_update_shutdown(&app->firmware_update);
    /* Before the controller it presses keys on. */
    mesh_app_control_close(&app->control);
    mesh_ui_controller_shutdown(&app->ui_controller);
    mesh_app_close_ui_cache_timer(app);
    if (app->ui_handshake_cache_path[0] != '\0') {
        mesh_ui_store_save(&app->ui_store, app->ui_handshake_cache_path);
        app->ui_handshake_cache_dirty = false;
    }
    mesh_ui_store_shutdown(&app->ui_store);
    if (app->ui_preferences_dirty && app->ui_preferences_path[0] != '\0') {
        mesh_ui_preferences_save(&app->ui_preferences, app->ui_preferences_path);
        app->ui_preferences_dirty = false;
    }
    inkwell_loop_shutdown(&app->loop);
}

/*
 * How long one turn of the foreground loop may spend in the event loop.
 *
 * The ordinary answer is the configured idle timeout, and the short one is for a handover that
 * is moving bytes. inkwell_loop_run() bounds the *whole call* rather than each wait, and it
 * only returns early when epoll falls idle - which during an install it never does, because the
 * progress bar being drawn re-arms a 33 ms frame timer for as long as it is on the screen. So a
 * turn costs the full second, and the BLE transfer, which is strictly one chunk per ACK per
 * tick, moves 512 bytes in that second: a 2.2 MB image took 74 minutes on a link that does it
 * in two.
 *
 * Shortening the turn rather than draining inside the tick keeps the property the deadline was
 * added for - see the comment over inkwell_loop_run(), where a self-rearming fd starving the
 * periodic work is exactly what went wrong before. A shorter bound starves nothing; it hands
 * control back sooner, and everything else on the loop is serviced on the way past.
 *
 * 20 ms is the number the CLI already pumps this same install at - see the loop around
 * mesh_firmware_ota_tick() in install_radio_firmware_ble(), src/main.c - so this is the HUD
 * catching up with the path that was always fast rather than a figure picked here. It is about
 * the link: a write and its notification are two connection intervals, and the install asks for
 * 7.5 ms ones, so a turn much shorter only spends wake-ups finding the same chunk in flight.
 */
#define MESH_APP_TRANSFER_TURN_MS 20

int mesh_app_turn_ms(const struct mesh_app *app) {
    if (app == NULL) {
        return 0;
    }
    const int configured = app->config.idle_timeout_ms;
    if (!mesh_firmware_update_holds_the_radio(&app->firmware_update)) {
        return configured;
    }
    /* Never *longer* than the caller asked for: 0 is already the shortest turn there is and
       stays the drain-and-return it means everywhere else. */
    if (configured == 0) {
        return 0;
    }
    /* Negative is "wait for an fd and keep going", which inkwell_loop_run() has no deadline
       to break out of - the one configuration where the transfer needs the bound imposed rather
       than lowered. */
    if (configured < 0) {
        return MESH_APP_TRANSFER_TURN_MS;
    }
    return configured < MESH_APP_TRANSFER_TURN_MS ? configured : MESH_APP_TRANSFER_TURN_MS;
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
    inkwell_signals_init(&app->signals, &app->loop);
    const struct inkcell_input_host ui_input_host = {
        .ctx = &app->loop,
        .add_fd = mesh_app_ui_add_fd,
        .remove_fd = mesh_app_ui_remove_fd,
        .request_stop = mesh_app_ui_request_stop,
    };
    /* Unless the backend is already reading them. A window delivers its own presses, and a
       host where /dev/input is readable would otherwise hand every one of them over twice. */
    if (!app->ui_backend_reads_input) {
        inkcell_input_init(&app->ui_input, &ui_input_host);
        inkcell_input_set_handler(&app->ui_input, mesh_app_on_ui_key, app);
    }
    if (app->config.ui_control_path[0] != '\0') {
        const int opened = mesh_app_control_open(&app->control, &app->loop, &app->ui_controller,
                                                 app->config.ui_control_path);
        if (opened < 0) {
            inkwell_log_warn("app", "UI control socket at %s did not open: %s",
                             app->config.ui_control_path, strerror(-opened));
        }
    }

    mesh_app_publish_ui_state(app);

    switch (app->config.run_mode) {
    case MESH_APP_RUN_SINGLE_POLL:
        inkwell_log_debug("app", "Running single poll with timeout %d ms",
                          app->config.idle_timeout_ms);
        mesh_app_publish_ui_state(app);
        result = inkwell_loop_run(&app->loop, app->config.idle_timeout_ms);
        if (result >= 0) {
            mesh_app_publish_ui_state(app);
        }
        break;
    case MESH_APP_RUN_FOREGROUND:
        inkwell_log_info("app", "Starting foreground event loop (timeout %d ms)",
                         app->config.idle_timeout_ms);
        /* Paint the first frame before any transport work: the store already has a refresh
           queued, and a zero timeout drains what is ready without waiting for more. */
        inkwell_loop_run(&app->loop, 0);
        while (true) {
            mesh_transport_registry_tick(&app->transport_registry);
            /* The updater's connection is watched by the event loop; this enforces its timeout
               and resumes a read that gave the loop back early. */
            mesh_updater_tick(&app->updater, inkwell_time_monotonic_ms());
            mesh_firmware_tick(&app->firmware, inkwell_time_monotonic_ms());
            /* One antenna: a download and a link cannot both have it, and the link is the one
               that loses - a Meshtastic node ends the connection after a second of silence,
               while a download only takes longer. Derived here rather than done at the install
               press so every route into a download releases it, and paired with the auto-connect
               guard above so nothing brings it back mid-download. */
            if (mesh_updater_holds_the_radio(&app->updater)) {
                mesh_app_release_other_link(NULL);
            }
            /*
             * The same hold, for the same antenna, taken by the other download - and it keeps
             * the cable.
             *
             * A serial link needs no antenna at all, so a USB install has no reason to drop the
             * radio it is about to send into DFU, and every reason not to: the arm goes out down
             * that link, and a link taken away for the length of the download would have to come
             * back before the press could finish. Over Bluetooth there is nothing to keep, which
             * is what makes MESH_FIRMWARE_UPDATE_READY a step on that bus and a no-op on this
             * one. Lifted when the image lands either way - see mesh_app_autoconnect() for why
             * the antenna and the radio are two questions.
             */
            if (mesh_firmware_update_holds_the_antenna(&app->firmware_update)) {
                mesh_app_release_other_link(app->firmware_update.path == MESH_FIRMWARE_PATH_USB
                                                ? mesh_serial_transport()
                                                : NULL);
            }
            mesh_app_firmware_update_tick(app, inkwell_time_monotonic_ms());
            /* After the transports have been pumped, so a link that dropped this turn has
               already cleared the config sync that this reads to decide whether to stay
               connected at all. */
            mesh_app_mqtt_tick(app, inkwell_time_monotonic_ms());
            /* Before auto-connect, not after: a retry starts the link over and clears the
               reason the last attempt failed. */
            (void)mesh_app_report_link_errors(app);
            mesh_app_autoconnect(app);
            mesh_app_publish_ui_state(app);
            result = inkwell_loop_run(&app->loop, mesh_app_turn_ms(app));
            if (result < 0) {
                break;
            }
            if (app->loop.stop_requested) {
                inkwell_log_info("app", "Event loop stop requested");
                break;
            }
            mesh_app_publish_ui_state(app);
        }
        break;
    default:
        inkwell_log_warn("app", "Unknown run mode %d, performing single poll",
                         app->config.run_mode);
        mesh_transport_registry_tick(&app->transport_registry);
        mesh_app_publish_ui_state(app);
        result = inkwell_loop_run(&app->loop, app->config.idle_timeout_ms);
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
                                 : NULL;
        mesh_ui_store_set_transport_status(
            &app->ui_store, status != NULL ? status : inkcell_str(MESH_STR_TRANSPORT_STOPPED));
    }

    inkcell_input_shutdown(&app->ui_input);
    inkwell_signals_shutdown(&app->signals);
    return result;
}
