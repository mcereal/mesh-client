#define _POSIX_C_SOURCE 200809L

/*
 * What the navigation model cannot do by itself: talk to the radio.
 *
 * One handler per mesh_ui_action_type, and a table naming them, reached from the UI controller.
 * Everything a handler needs from the rest of the app - connecting a link, queueing a settings
 * write, naming a peer - is in app_internal.h, so a handler stays a translation of one press
 * rather than growing logic of its own.
 *
 * The unit is the handler, which is worth stating because this was one 1200-line switch and the
 * arm was not a unit of anything: an arm shared its locals with every other arm, so a `toast`
 * filled in twelve arms up was in scope in this one, and the two longest had grown sub-switches
 * nothing outside them could see. A handler declares the `toast` and `now` it uses and cannot
 * reach another's, and what it may assume before it runs - today only that there is a BLE
 * adapter - is a column in the table rather than four lines repeated at the top of it.
 */

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"

#include "app_internal.h"

#include "inkwell/runtime/crash.h"
#include "mesh/core/version.h"
#include "mesh/geo/coords.h"
#include "mesh/i18n/strings.h"
#include "mesh/proto/channel_url.h"
#include "mesh/proto/contact_url.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_hci.h"
#include "mesh/transport/serial.h"
#include "mesh/transport/tcp.h"
#include "mesh/ui/contact_share.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/preferences.h"
#include "mesh/utils/crash.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- installing the radio's firmware ------------------------------------------------------ */

/*
 * The four things src/core/firmware/firmware_update.c cannot do for itself, because doing any of
 * them would mean knowing what a session or a transport registry is.
 *
 * Both arms *queue*: the admin queue drains from the session's tick, which a transport calls
 * from its own, so what these return is "it is on its way out" rather than "the radio has it".
 * That is the same split every other admin verb here lives with, and it is why the link is not
 * released until the handover has left arming.
 */
/* The loader wants 7.5 ms and BlueZ has no D-Bus call for it, so it is a raw HCI command - see
   mesh/transport/ble_hci.h. A wrapper because the hook's shape is (device, address) and the
   parameters are this one connection's business rather than the caller's. */
static int mesh_app_firmware_interval(int hci_dev, const char *address) {
    return mesh_ble_hci_request_interval(hci_dev, address, &mesh_ble_hci_ota_params);
}

/*
 * Both arms answer the session's question - "how many did you queue" - and the install's hook
 * answers its own: 0, or -errno. So both of them translate, and the translation is not a
 * tidying. A queued verb counts 1, which the install reads as a refusal: it says the radio
 * would not take the request, stops, and leaves the radio it really did arm sitting in a
 * bootloader with nothing on its way to it.
 */
static int mesh_app_firmware_arm_usb(void *userdata) {
    struct mesh_app *const app = (struct mesh_app *)userdata;
    const int queued = mesh_session_radio_action(&app->session, MESH_ADMIN_ENTER_DFU_MODE);
    return queued < 0 ? queued : 0;
}

static int mesh_app_firmware_arm_ble(void *userdata, const uint8_t sha256[32]) {
    struct mesh_app *const app = (struct mesh_app *)userdata;
    const int queued = mesh_session_request_ble_ota(&app->session, sha256);
    return queued < 0 ? queued : 0;
}

/*
 * Whether the radio to arm on is back - **the same one**, not any one.
 *
 * A connected link with metadata was the first answer and it was wrong in the one way that
 * matters here. The BLE path takes the antenna down for the length of the download, so by the
 * time this is asked auto-connect has been running free for half a minute: it may have landed
 * on a different node, and a USB radio plugged in meanwhile wins its priority outright. Arming
 * whatever answered would put the `ota_request` down a session belonging to another radio while
 * the job still held the first one's board, image and address - so the wrong radio goes into a
 * loader, holding the hash of an image that is not for it.
 *
 * Three questions, because it takes three to be sure it is the same radio: the bus the job is
 * for, the address or port it was on, and the model the image was chosen for. The model alone
 * is not enough - two Heltec V3s on one desk share it - and the address alone is not enough
 * either, since it is the one field a reconnect to a *different* transport does not compare
 * against anything.
 *
 * `has_metadata` is what makes the last of those answerable: a link whose handshake has not
 * landed has not said what it is yet, and this is deliberately not the place to guess.
 */
static bool mesh_app_firmware_radio_ready(void *userdata) {
    struct mesh_app *const app = (struct mesh_app *)userdata;
    const struct mesh_firmware_update *const update = &app->firmware_update;
    const char *const identifier = mesh_app_connected_identifier();
    /* The *link's* metadata, not the Settings tab's: what this asks is whether the radio now on
       the other end is the one the image was chosen for, and the tab may be describing a node
       over the mesh that has nothing to do with this cable. */
    const meshtastic_DeviceMetadata *const metadata =
        mesh_radio_settings_link_metadata(mesh_session_settings(&app->session));
    if (identifier == NULL || metadata == NULL) {
        return false;
    }
    if (mesh_app_firmware_bus() != update->path) {
        return false;
    }
    if (update->hw_model != 0U && (uint32_t)metadata->hw_model != update->hw_model) {
        return false;
    }
    /* The USB path names the port by the transport's own id rather than by the row's label, for
       the reason the install does - see where `where` is filled in below. An empty `where` is
       the recovery case and matches anything, which is the same "any" the handover means. */
    const char *const now_at = update->path == MESH_FIRMWARE_PATH_USB
                                   ? mesh_serial_transport_connected_id(mesh_serial_transport())
                                   : identifier;
    if (update->where[0] == '\0') {
        return true;
    }
    return now_at != NULL && strcasecmp(now_at, update->where) == 0;
}

/*
 * The radio has said everything it is going to; stop using the bus.
 *
 * How much of the bus depends on which one, and the two are not the same amount - see below.
 */
static void mesh_app_firmware_release_link(void *userdata) {
    struct mesh_app *const app = (struct mesh_app *)userdata;
    if (app->firmware_update.path == MESH_FIRMWARE_PATH_USB) {
        /*
         * The tty, and only the tty. The registry goes on ticking on purpose: the radio's port
         * disappearing is what the serial transport notices, and there is no second scan to get
         * out of the way of - a UF2 bootloader is found by walking sysfs, not over a bus this
         * client is sharing. Stopping everything here would be taking away the one thing
         * watching for the board to come back.
         */
        inkwell_log_info("ui", "Releasing the serial link for the firmware install");
        (void)mesh_serial_transport_disconnect(mesh_serial_transport());
        return;
    }
    if (app->firmware_transports_stopped) {
        return;
    }
    /*
     * Over Bluetooth it is the whole registry, and that is the point: the thing on the other end
     * from here is a loader's GATT, and a transport left running would scan for radios beside an
     * install doing its own discovery and then connect to the one device it found - which is the
     * radio being written to. The install has its own D-Bus connection for the neighbouring
     * reason, and gets the adapter to itself by this.
     *
     * Restarted when the job ends, by the completion below.
     */
    inkwell_log_info("ui", "Stopping transports for the firmware install");
    mesh_transport_registry_stop_all(&app->transport_registry);
    app->firmware_transports_stopped = true;
}

/*
 * The five of them together: what the press hands over, and what a suite can hold to the
 * contracts above without standing an install up around them.
 */
struct mesh_firmware_update_hooks mesh_app_firmware_hooks(struct mesh_app *app) {
    const struct mesh_firmware_update_hooks hooks = {
        .arm_usb = mesh_app_firmware_arm_usb,
        .arm_ble = mesh_app_firmware_arm_ble,
        .radio_ready = mesh_app_firmware_radio_ready,
        .release_link = mesh_app_firmware_release_link,
        .request_interval = mesh_app_firmware_interval,
        .userdata = app,
    };
    return hooks;
}

/*
 * A radio we have just reflashed has to prove its bond still works.
 *
 * This client is the only thing that changes a radio's own half of a BLE bond, and on an ESP32
 * a firmware change rotates its keys: after 2.8.0.47db0e3 a Heltec V3 answered every connect
 * with le-connection-abort-by-local, because the LTK on this side was one the radio no longer
 * had. Nothing in the link layer can say so. BlueZ reports the device Paired - our half of the
 * bond is intact and on disk - so mesh_ble_do_connect() takes the bonded branch, the encryption
 * it sets up is refused, and the only sentence the client has left is "connect failed". The way
 * out is to forget the node and pair again, which is not something a user can be expected to
 * derive from those two words.
 *
 * So the install arms a watch instead of an answer. A radio that comes back inside it kept its
 * bond and nothing happens - dropping every bond after every update would charge a PIN to the
 * upgrades that did not need one. A radio that does not come back has a bond this client can
 * see is dead, and dropping it is what turns the dead end into LINK_NEEDS_PAIRING, which says
 * which button to press.
 *
 * USB is deliberately not watched. A serial radio has no bond to lose, and the same reflash
 * over a cable leaves nothing behind to go stale.
 */
#define MESH_APP_FIRMWARE_BOND_GRACE_MS 60000U
/* How long to leave a removal that failed for a reason that can pass before trying it again. */
#define MESH_APP_FIRMWARE_BOND_RETRY_MS 5000U

void mesh_app_firmware_watch_bond(struct mesh_app *app, const struct mesh_firmware_update *update,
                                  uint64_t now) {
    if (app == NULL || update == NULL || update->path != MESH_FIRMWARE_PATH_BLE) {
        return;
    }
    /* The OTA's own answer where it has one: it derives the radio's address from the loader's
       when a job was resumed against a board already in a loader, and that is the case where
       the press never knew which radio this was. */
    const char *const address =
        update->ble.radio_address[0] != '\0' ? update->ble.radio_address : update->where;
    if (address[0] == '\0') {
        return;
    }
    inkwell_str_copy(app->firmware_bond_watch, sizeof app->firmware_bond_watch, address);
    app->firmware_bond_watch_until_ms = now + MESH_APP_FIRMWARE_BOND_GRACE_MS;
    inkwell_log_info("ui", "Watching %s for %u s to see whether its bond survived the update",
                     address, (unsigned)(MESH_APP_FIRMWARE_BOND_GRACE_MS / 1000U));
}

/* One turn of that watch. Returns true when it dropped a bond, which is only ever once. */
bool mesh_app_firmware_settle_bond(struct mesh_app *app, const char *connected, uint64_t now) {
    if (app == NULL || app->firmware_bond_watch[0] == '\0') {
        return false;
    }
    /* Back on the link, so the bond outlived the firmware that shared it. Handed in rather than
       read from mesh_app_connected_identifier() here: what this answers is a question about two
       addresses and a clock, and taking them as arguments is what lets a suite ask it. */
    if (connected != NULL && strcasecmp(connected, app->firmware_bond_watch) == 0) {
        inkwell_log_info("ui", "%s came back after its update; its bond is intact",
                         app->firmware_bond_watch);
        app->firmware_bond_watch[0] = '\0';
        app->firmware_bond_watch_until_ms = 0U;
        return false;
    }
    /*
     * Some *other* radio is on the link, so this bond has not been tested and the clock has no
     * business running.
     *
     * mesh_app_autoconnect() returns early whenever a link is up - one radio at a time - so a
     * serial node plugged in after the update, or a TCP target that answered, means BLE is never
     * reached for and the watched address could not match however long this waited. Expiring
     * against that would throw away a bond nothing had found fault with, and charge a PIN for
     * it. Re-armed rather than merely held, so the radio gets a whole grace from the moment the
     * bus is free to try it.
     */
    if (connected != NULL) {
        app->firmware_bond_watch_until_ms = now + MESH_APP_FIRMWARE_BOND_GRACE_MS;
        return false;
    }
    if (now < app->firmware_bond_watch_until_ms) {
        return false;
    }

    struct mesh_transport *const ble = mesh_ble_transport();
    const int dropped =
        ble != NULL ? mesh_ble_transport_forget(ble, app->firmware_bond_watch) : -ENODEV;
    /*
     * -ENOENT is bluetoothd's DoesNotExist, which is the state this was trying to reach: no bond
     * on that address any more. Anything else can pass - an adapter still coming back from the
     * install, a transport between its stop and its start - and an adapter that is away has not
     * lost the bond it persisted, so clearing the watch on one would leave the stale key in
     * place and the reconnects failing, which is what this exists to end. Retried instead, on a
     * slow cadence, because the watch is only ever armed by an install that has just finished.
     */
    if (dropped < 0 && dropped != -ENOENT) {
        app->firmware_bond_watch_until_ms = now + MESH_APP_FIRMWARE_BOND_RETRY_MS;
        inkwell_log_info("ui", "Could not drop %s's bond yet (%d); retrying",
                         app->firmware_bond_watch, dropped);
        return false;
    }

    const bool removed = dropped == 0;
    if (removed) {
        /* Deliberately *not* dropped from the preferred devices, unlike the Devices tab's
           Forget. That press means "stop reaching for this radio"; this one means "reach for it,
           but pair first" - it is the radio the user just spent minutes updating. */
        inkwell_log_warn("ui",
                         "%s did not come back after its update; dropped the stale bond so it can "
                         "be paired with again",
                         app->firmware_bond_watch);
    } else {
        inkwell_log_info("ui", "%s did not come back and had no bond left to drop",
                         app->firmware_bond_watch);
    }
    app->firmware_bond_watch[0] = '\0';
    app->firmware_bond_watch_until_ms = 0U;
    return removed;
}

void mesh_app_firmware_update_done(void *userdata, const struct mesh_firmware_update *update) {
    struct mesh_app *const app = (struct mesh_app *)userdata;
    const uint64_t now = inkwell_time_monotonic_ms();
    char toast[MESH_UI_NAV_TOAST_MAX];

    if (app->firmware_transports_stopped) {
        /* Whatever happened, the bus is ours to give back: a radio that came through this is
           one auto-connect should be reaching for again, and one that did not is still a device
           the Devices tab has to be able to show. */
        (void)mesh_transport_registry_start_all(&app->transport_registry, &app->config, &app->loop);
        app->firmware_transports_stopped = false;
    }

    if (update->state == MESH_FIRMWARE_UPDATE_DONE) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_FIRMWARE_INSTALLED,
                           update->release.version);
        inkwell_log_info("ui", "Radio firmware %s installed", update->release.version);
        /*
         * The check's answer was about the firmware this radio *was* running. Dropping it means
         * the rows go back to "not checked" rather than going on offering an install of what is
         * now installed - and the next handshake carries the new version, which is what a fresh
         * check would compare against anyway.
         */
        mesh_firmware_forget(&app->firmware);
        mesh_app_firmware_watch_bond(app, update, now);
    } else {
        /* The radio's own words where it supplied any, our name for the category where not.
           Untranslated either way, like a log line. */
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_FIRMWARE_FAILED,
                           update->detail[0] != '\0'
                               ? update->detail
                               : mesh_firmware_update_error_name(update->error));
        inkwell_log_warn("ui", "Radio firmware install failed: %s (%s)",
                         mesh_firmware_update_error_name(update->error), update->detail);
    }
    /* Posted rather than set: the press that started this was minutes ago and whatever is on
       the screen now is the answer to something else. */
    mesh_ui_store_post_toast(&app->ui_store, now, toast);
}

void mesh_app_firmware_update_tick(struct mesh_app *app, uint64_t now) {
    if (app == NULL) {
        return;
    }
    /*
     * What the radio said since the last turn, handed to an install that is waiting for it.
     *
     * Only while something is running, and only once per notification: the sequence is what
     * stops a "Rebooting to BLE OTA" from an earlier install - or from another client on the
     * same radio - being replayed into the arming step of the next one.
     */
    if (mesh_firmware_update_busy(&app->firmware_update)) {
        const struct mesh_client_notification *const note =
            mesh_session_notification(&app->session);
        if (note != NULL && note->seq != app->firmware_notification_seq) {
            app->firmware_notification_seq = note->seq;
            mesh_firmware_update_radio_said(&app->firmware_update, note->text);
        }
    }
    mesh_firmware_update_tick(&app->firmware_update, now);
    if (mesh_app_firmware_settle_bond(app, mesh_app_connected_identifier(), now)) {
        char toast[MESH_UI_NAV_TOAST_MAX];
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_FIRMWARE_REPAIR,
                           app->firmware_update.release.version);
        mesh_ui_store_post_toast(&app->ui_store, now, toast);
    }
}

/* ---- one handler per press ----------------------------------------------------------------- */

/*
 * The name to put in a toast for the node an action names.
 *
 * Nine of the handlers below open with this and the reading is the same every time: the session
 * roster is the authority, and mesh_app_format_peer_name() falls back to the "!hex" id when it
 * has no short name to give. A wrapper so an arm says which node it is talking about rather than
 * how a node is spelled.
 */
static void action_peer_name(const struct mesh_app *app, uint32_t node, char *out, size_t out_len) {
    mesh_app_format_peer_name(mesh_session_handshake(&app->session), node, out, out_len);
}

static void on_connect(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    /* The kind, in the log's own words. Inline rather than behind a helper because
       scripts/check-strings.py exempts a literal inside inkcell_log() and nowhere else, and
       these three are log words - untranslated, like every other line this file writes. */
    inkwell_log_info("ui", "Connect to %s (%s) requested from the device", action->identifier,
                     action->kind == (uint8_t)MESH_UI_DEVICE_SERIAL ? "usb"
                     : action->kind == (uint8_t)MESH_UI_DEVICE_TCP  ? "network"
                                                                    : "ble");
    /*
     * A network link is deliberately not remembered here, for the reason
     * mesh_app_publish_ui_state() does not remember it either: this history is what
     * auto-connect ranks a *scan* with, and a host is in no scan - configuration already
     * holds it. Recorded, an address would sit in the eight slots a real radio needs,
     * matching nothing that could ever be scanned.
     */
    if (action->kind != (uint8_t)MESH_UI_DEVICE_TCP) {
        mesh_app_note_connected_device(app, action->identifier, action->kind);
    }
    /* Asking for a radio lifts a hold an earlier disconnect put on auto-connect. */
    app->autoconnect_held = false;
    app->autoconnect_failures = 0U;
    app->autoconnect_retry_at_ms = 0U;

    /* A user pick beats whatever auto-connect is doing or has done, on either link. */
    const int result = mesh_app_link_connect(app, action->identifier, action->kind);
    /*
     * A host gets a preference of its own instead of a slot in the list above, and it is
     * kept whether or not the radio answered: a connect that timed out is a radio that is
     * off or a Brick on the wrong WiFi, and the address is still the one the user wrote
     * down. What it is not kept for is a target the transport refused outright - a typo, a
     * name, the link disabled by configuration - because the link never adopted one of
     * those and the file would then name a host nothing is reaching for.
     *
     * So the test is that *this* press's address is the one the transport ended up pointed
     * at, rather than that the transport is pointed at anything: with --tcp-host naming a
     * host and nothing saved yet, "the link has a target" is true before the press, and a
     * refused press would have written the flag's host into the file - persisting a choice
     * the user did not make out of a press that failed.
     */
    const char *adopted = mesh_tcp_transport_configured_target(mesh_tcp_transport());
    if (action->kind == (uint8_t)MESH_UI_DEVICE_TCP && adopted != NULL &&
        strcmp(adopted, action->identifier) == 0 &&
        strcmp(app->ui_preferences.network_host, adopted) != 0) {
        inkwell_str_copy(app->ui_preferences.network_host, sizeof app->ui_preferences.network_host,
                         adopted);
        app->ui_preferences_dirty = true;
    }
    if (result == 0 || result == -EALREADY || result == -EINPROGRESS) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_CONNECTING, action->identifier);
        /* BLE resolves services from tick(), so a 0 here is not yet a connection. Arm the
           error report so whatever goes wrong next reaches the screen. */
        app->ui_report_link_error = true;
    } else if (mesh_transport_registry_take_error(&app->transport_registry, toast, sizeof toast)) {
        inkwell_log_warn("ui", "Connect to %s failed: %s (%d)", action->identifier, toast, result);
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_CONNECT_FAILED, result);
        inkwell_log_warn("ui", "Connect to %s failed: %d", action->identifier, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_send_text(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    const bool broadcast = (action->dest == MESH_MESSAGE_BROADCAST_ADDR);
    uint32_t packet_id = 0U;
    /*
     * Three sends behind one action: a reaction names its target and asks for nothing back,
     * a reply names its target and is a message like any other, and everything else is a
     * message with no target. The session tells them apart on the wire; the difference
     * here is which of the three the nav filled in.
     */
    const int result =
        action->is_reaction
            ? mesh_session_send_reaction(&app->session, action->dest, action->channel, action->text,
                                         action->reply_id, &packet_id)
            : mesh_session_send_reply(&app->session, action->dest, action->channel, action->text,
                                      !broadcast, action->reply_id, &packet_id);
    if (result == 0 && action->is_reaction) {
        /* A tapback has no bubble and nothing to wait for, so it is not watched: there is
           no delivery mark for a report to land on. */
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_REACTION_SENT));
        inkwell_log_info("ui", "Reacted \"%s\" to packet %u in %s", action->text, action->reply_id,
                         app->ui_store.nav.target_name);
    } else if (result == 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SENT_TO,
                           app->ui_store.nav.target_name);
        if (action->reply_id != 0U) {
            inkwell_log_info("ui", "Sent \"%s\" to %s (packet %u, replying to %u)", action->text,
                             app->ui_store.nav.target_name, packet_id, action->reply_id);
        } else {
            inkwell_log_info("ui", "Sent \"%s\" to %s (packet %u)", action->text,
                             app->ui_store.nav.target_name, packet_id);
        }
        mesh_app_watch_sent(app, packet_id, app->ui_store.nav.target_name);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SEND_FAILED, result);
        inkwell_log_warn("ui", "Send to %s failed: %d", app->ui_store.nav.target_name, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

/*
 * One message that came back undelivered, sent again.
 *
 * A new packet with a new id rather than the old one put back on the air: a Routing reply has
 * already been seen for that id, so re-using it would hand every node on the path a duplicate
 * of something it has already forwarded and answered - and this client would have two log
 * entries claiming the same id, which is what mesh_message_log_find() resolves by taking the
 * newest. The words, the destination, the channel and whatever it was replying to are the
 * same; nothing else about it is.
 *
 * The failed bubble stays. The transcript is a record of what happened on the air and that
 * attempt happened, so the retry appears under it as its own message - which is also the only
 * honest thing to draw while the second one is still pending.
 */
static void on_resend(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    const bool broadcast = (action->dest == MESH_MESSAGE_BROADCAST_ADDR);
    uint32_t packet_id = 0U;
    /* Never a reaction: one is drawn on the message it names rather than as a bubble, so it is
       filtered out of the thread and can never be the row under the cursor. */
    const int result =
        mesh_session_send_reply(&app->session, action->dest, action->channel, action->text,
                                !broadcast, action->reply_id, &packet_id);
    if (result == 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_RESENT_TO,
                           app->ui_store.nav.target_name);
        inkwell_log_info("ui", "Resent \"%s\" to %s as packet %u (packet %u went undelivered)",
                         action->text, app->ui_store.nav.target_name, packet_id, action->number);
        /* Watched like any other send, so the retry's own result reaches the user. The failed
           attempt has already been reported and is no longer watched. */
        mesh_app_watch_sent(app, packet_id, app->ui_store.nav.target_name);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_SEND_FAILED, result);
        inkwell_log_warn("ui", "Resend to %s failed: %d", app->ui_store.nav.target_name, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_refresh_settings(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    const int result = mesh_session_refresh_settings(&app->session);
    if (result > 0 && action->edit_count > 0U) {
        inkcell_str_format_plural(toast, sizeof toast, MESH_STR_TOAST_REFRESH_EDITS_ONE,
                                  action->edit_count, result, (unsigned)action->edit_count);
    } else if (result > 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_REFRESHING, result);
    } else if (result == 0) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_REFRESH_IN_PROGRESS));
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_REFRESH_FAILED, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

/*
 * Point the Settings tab at another node's radio, or bring it back to our own.
 *
 * One handler for both directions because the verb is one verb: `dest` says which radio, and 0
 * is the one on the end of the link. What it says afterwards is the new subject rather than the
 * refresh it queued - the round trips are the progress bar's business, and on a remote target
 * there are nearly thirty of them over the air, so a count here would be promising a wait
 * rather than reporting a change.
 */
static void on_set_admin_target(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    action_peer_name(app, action->dest, name, sizeof name);
    const int result = mesh_session_set_admin_dest(&app->session, action->dest);
    if (result >= 0 && action->dest == 0U) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_ADMIN_LOCAL));
    } else if (result >= 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_ADMIN_REMOTE, name);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else if (result == -EINVAL || result == -ENOENT) {
        /* The one thing this side can check: an admin request to a remote node is sealed to
           that node's key, and a node that has never broadcast one cannot be addressed at all.
           Said as what is missing rather than as a press that failed. */
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_ADMIN_NO_KEY, name);
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_ADMIN_FAILED, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_save_settings(struct mesh_app *app, const struct mesh_ui_action *action) {
    const uint64_t now = inkwell_time_monotonic_ms();

    mesh_app_save_settings(app, action, now);
}

/*
 * Reboot, shutdown, the three resets and the backup trio: the rows that are one AdminMessage
 * and an announcement of what was asked for.
 *
 * A table for the reason the ones in src/ui/tables/actions.c are: the row that was confirmed, the
 * request it becomes and the words it answers with belong on one line, where they can be read
 * against the settings row that raised the press. Nothing here waits for an answer - the radio
 * acts a few seconds after acking and takes the link with it - so `asked` is phrased as the
 * request rather than as the outcome. A shutdown in particular has no reconnect to promise: the
 * radio has to be switched on by hand, so it says so rather than leaving auto-connect to look
 * broken while it retries a node that is off.
 */
struct radio_admin_verb {
    enum mesh_ui_settings_action row;
    enum mesh_admin_request_kind kind;
    inkcell_str_id asked;
    /* A restore is the one that changes what the radio holds, and the settings it changes are
       the ones this tab is showing - so it is followed by a refresh rather than left to a
       screen that would keep drawing the values it had before. */
    bool refresh_after;
};

static const struct radio_admin_verb k_radio_admin_verbs[] = {
    {MESH_UI_SETTINGS_ACTION_REBOOT, MESH_ADMIN_REBOOT, MESH_STR_TOAST_REBOOTING, false},
    {MESH_UI_SETTINGS_ACTION_SHUTDOWN, MESH_ADMIN_SHUTDOWN, MESH_STR_TOAST_SHUTTING_DOWN, false},
    /* Says what it did *not* touch as well: the Brick's own roster outliving the reset is the
       difference between the Status screen's 2 nodes and the Nodes tab's 81, and the row below
       the one just pressed is what clears it. */
    {MESH_UI_SETTINGS_ACTION_RESET_NODEDB, MESH_ADMIN_RESET_NODEDB, MESH_STR_TOAST_NODEDB_RESET,
     false},
    {MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG, MESH_ADMIN_FACTORY_RESET_CONFIG,
     MESH_STR_TOAST_FACTORY_CONFIG, false},
    {MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE, MESH_ADMIN_FACTORY_RESET_DEVICE,
     MESH_STR_TOAST_FACTORY_DEVICE, false},
    {MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG, MESH_ADMIN_BACKUP_PREFERENCES, MESH_STR_TOAST_BACKED_UP,
     false},
    {MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG, MESH_ADMIN_RESTORE_PREFERENCES,
     MESH_STR_TOAST_RESTORED, true},
    {MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP, MESH_ADMIN_REMOVE_BACKUP_PREFERENCES,
     MESH_STR_TOAST_BACKUP_REMOVED, false},
};

/* NULL for a row the nav should never have confirmed, which is what the old switch's `default`
   arm meant: the press is dropped rather than guessed at. */
static const struct radio_admin_verb *radio_admin_verb_for(uint32_t row) {
    for (size_t i = 0; i < sizeof k_radio_admin_verbs / sizeof k_radio_admin_verbs[0]; ++i) {
        if ((uint32_t)k_radio_admin_verbs[i].row == row) {
            return &k_radio_admin_verbs[i];
        }
    }
    return NULL;
}

/*
 * Asking a Store & Forward router for what we missed. Not an AdminMessage at all - it is a
 * packet to another node on the mesh - so it leaves before the admin queue, the way the
 * fixed-position pair does.
 *
 * The toast says which of the two things the press did, because they take different amounts of
 * time: with a router already known it is one packet and an answer, and with none it is a
 * broadcast ping first and up to half a minute of waiting. Nothing here announces the messages
 * themselves; they arrive in the transcript, and the section's own row counts them.
 */
static void radio_request_history(struct mesh_app *app, uint64_t now) {
    const struct mesh_store_forward *before = mesh_session_store_forward(&app->session);
    const bool knew_router = before != NULL && before->router != 0U;
    char name[MESH_UI_STORE_FORWARD_NAME_MAX];
    inkwell_str_copy(name, sizeof name, app->ui_store.settings.store_forward.router_name);

    const int result = mesh_session_request_history(&app->session);
    char toast[MESH_UI_NAV_TOAST_MAX];
    if (result == -EBUSY) {
        inkwell_str_copy(toast, sizeof toast, inkcell_str(MESH_STR_TOAST_HISTORY_RUNNING));
    } else if (result < 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_HISTORY_FAILED, result);
    } else if (knew_router) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_HISTORY_ASKED, name);
    } else {
        inkwell_str_copy(toast, sizeof toast, inkcell_str(MESH_STR_TOAST_HISTORY_LOOKING));
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_radio_action(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();
    const enum mesh_ui_settings_action row = (enum mesh_ui_settings_action)action->number;

    /* Three rows reach this verb without being admin requests at all, so they are answered
       ahead of the table. The two fixed-position rows are radio actions but not destructive
       ones: they are a save the user pressed for, they read the coordinate rows above them, and
       they are announced through the same "Saving ..." machinery a section save uses. */
    if (row == MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION ||
        row == MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION) {
        mesh_app_save_fixed_position(app, action, now);
        return;
    }
    /* Ham mode is the same shape one step further along: a row that reads the three above it,
       announced like a save, and the only one of them behind the confirm sheet. */
    if (row == MESH_UI_SETTINGS_ACTION_SET_HAM_MODE) {
        mesh_app_save_ham_mode(app, action, now);
        return;
    }
    if (row == MESH_UI_SETTINGS_ACTION_REQUEST_HISTORY) {
        radio_request_history(app, now);
        return;
    }

    const struct radio_admin_verb *const verb = radio_admin_verb_for(action->number);
    if (verb == NULL) {
        return;
    }
    const int result = mesh_session_radio_action(&app->session, verb->kind);
    if (result > 0) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(verb->asked));
        if (verb->refresh_after) {
            (void)mesh_session_refresh_settings(&app->session);
        }
    } else if (result == 0) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_ALREADY_REQUESTED));
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_REQUEST_FAILED, result);
        inkwell_log_warn("ui", "Radio action %u failed: %d", (unsigned)action->number, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_forget_nodes(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    /*
     * The two rows in Radio actions that ask nothing of the radio. The roster is this
     * client's - it outlives the link on purpose, because the radio's NodeDB holds 80
     * entries and evicts - so emptying it is a local edit, works with no link at all, and
     * says how many went rather than leaving the user to count the list.
     *
     * Published here rather than on the next turn, for the reason the theme switch is:
     * the frame this press draws is the answer, and it has to show the shorter list.
     */
    const bool all = (action->number != 0U);
    const int dropped = mesh_session_forget_nodes(&app->session, !all);
    if (dropped > 0) {
        inkcell_str_format_plural(toast, sizeof toast, MESH_STR_TOAST_FORGOT_NODES_ONE,
                                  (uint32_t)dropped, dropped);
        inkwell_log_info("ui", "Forgot %d cached node%s from Settings (%s)", dropped,
                         dropped == 1 ? "" : "s", all ? "all" : "off-radio only");
    } else if (dropped == 0) {
        snprintf(toast, sizeof toast, "%s",
                 inkcell_str(all ? MESH_STR_TOAST_NOTHING_CACHED : MESH_STR_TOAST_ALL_ON_RADIO));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_FORGET_FAILED, dropped);
        inkwell_log_warn("ui", "Forget nodes failed: %d", dropped);
    }
    /* The hint that sent the user here compares each sync against the last, so the press
       that acts on it has to move that baseline too: without this, a divergence the user
       has just cleared would still be the number the next sync is measured against. */
    app->ui_nodes_off_radio_seen = mesh_session_forgettable_nodes(&app->session, true);
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
    mesh_app_publish_ui_state(app);
}

static void on_toggle_favorite(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    const bool favorite = (action->number != 0U);
    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    action_peer_name(app, action->dest, name, sizeof name);
    const int result = mesh_session_set_node_favorite(&app->session, action->dest, favorite);
    if (result > 0) {
        inkcell_str_format(toast, sizeof toast,
                           favorite ? MESH_STR_TOAST_PINNED : MESH_STR_TOAST_UNPINNED, name);
        inkwell_log_info("ui", "%s node 0x%08x from the Nodes tab",
                         favorite ? "Pinned" : "Unpinned", action->dest);
    } else if (result == 0) {
        inkcell_str_format(
            toast, sizeof toast,
            favorite ? MESH_STR_TOAST_ALREADY_PINNED : MESH_STR_TOAST_ALREADY_UNPINNED, name);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else if (result == -ENOENT) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NODE_GONE));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_PIN_FAILED, result);
        inkwell_log_warn("ui", "Favorite for 0x%08x failed: %d", action->dest, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_request_node_info(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    action_peer_name(app, action->dest, name, sizeof name);
    const int result = mesh_session_request_node_info(&app->session, action->dest);
    if (result == 0) {
        /* Nothing here can promise an answer: the node may be out of range, asleep, or
           simply slow, and no ack comes back for the request itself. */
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_ASKED_NAME, name);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else if (result == -EAGAIN) {
        /* Our own owner record has not landed yet, and sending a placeholder would erase
           this node's name on whoever received it. */
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_STILL_SYNCING));
    } else if (result == -EINVAL) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_CANNOT_ASK));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_REQUEST_FAILED, result);
        inkwell_log_warn("ui", "NodeInfo request for 0x%08x failed: %d", action->dest, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_request_reading(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    const bool position = (action->type == MESH_UI_ACTION_REQUEST_POSITION);
    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    action_peer_name(app, action->dest, name, sizeof name);
    const int result = position ? mesh_session_request_position(&app->session, action->dest)
                                : mesh_session_request_telemetry(&app->session, action->dest);
    if (result == 0) {
        /* Nothing here can promise an answer either: the request carries no want_ack, and a
           node that is out of range, asleep or simply not equipped answers nothing. */
        inkcell_str_format(
            toast, sizeof toast,
            position ? MESH_STR_TOAST_ASKED_POSITION : MESH_STR_TOAST_ASKED_TELEMETRY, name);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else if (result == -EINVAL) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_CANNOT_ASK));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_REQUEST_FAILED, result);
        inkwell_log_warn("ui", "%s request for 0x%08x failed: %d",
                         position ? "Position" : "Telemetry", action->dest, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_toggle_ignore(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    const bool ignored = (action->number != 0U);
    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    action_peer_name(app, action->dest, name, sizeof name);
    const int result = mesh_session_set_node_ignored(&app->session, action->dest, ignored);
    if (result > 0) {
        /* Said as what it does to the traffic, not as a preference that was recorded. */
        inkcell_str_format(toast, sizeof toast,
                           ignored ? MESH_STR_TOAST_IGNORING : MESH_STR_TOAST_UNIGNORING, name);
        inkwell_log_info("ui", "%s node 0x%08x from the Nodes tab",
                         ignored ? "Ignoring" : "Unignoring", action->dest);
    } else if (result == 0) {
        inkcell_str_format(
            toast, sizeof toast,
            ignored ? MESH_STR_TOAST_ALREADY_IGNORED : MESH_STR_TOAST_ALREADY_UNIGNORED, name);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else if (result == -ENOENT) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NODE_GONE));
    } else if (result == -EINVAL) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_CANNOT_IGNORE));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_IGNORE_FAILED, result);
        inkwell_log_warn("ui", "Ignore for 0x%08x failed: %d", action->dest, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_toggle_mute(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    action_peer_name(app, action->dest, name, sizeof name);
    const int result = mesh_session_toggle_node_muted(&app->session, action->dest);
    if (result == 0) {
        /* Already on its way. Two local flips for one toggle on the wire would leave the
           row stating the opposite of what the radio is about to do. */
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_MUTE_REQUESTED));
    } else if (result > 0) {
        /* The session flipped the cached flag on the way through, so what it now holds is
           what we asked the radio for. */
        const struct mesh_ui_node_summary *node =
            mesh_ui_node_detail_find(&app->ui_store.handshake, action->dest);
        const bool muted = node != NULL ? node->is_muted : true;
        inkcell_str_format(toast, sizeof toast,
                           muted ? MESH_STR_TOAST_MUTED : MESH_STR_TOAST_UNMUTED, name);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else if (result == -ENOENT) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NODE_GONE));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_MUTE_FAILED, result);
        inkwell_log_warn("ui", "Mute for 0x%08x failed: %d", action->dest, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_remove_node(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    action_peer_name(app, action->dest, name, sizeof name);
    const int result = mesh_session_remove_node(&app->session, action->dest);
    if (result > 0) {
        /* Says how it comes back, because the row that would have undone it has gone with
           the node. */
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_REMOVED_NODE, name);
        inkwell_log_info("ui", "Removed node 0x%08x from the Nodes tab", action->dest);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else if (result == -ENOENT) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NODE_GONE));
    } else if (result == -EINVAL) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_REMOVE_SELF));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_REMOVE_FAILED, result);
        inkwell_log_warn("ui", "Remove of 0x%08x failed: %d", action->dest, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

/*
 * One bubble, out of all four places it lives.
 *
 * The conversation delete below explains the first three - the transport's ring, the history
 * read back at startup, and the store - and the fourth is the card's own transcript, which is
 * the one that would otherwise put the message back the next time the reader opened the thread
 * rather than merely on the next publish.
 *
 * The count reported is the store's, because that is what the reader was looking at. The other
 * three can legitimately differ: the ring may have evicted the message already, the restored
 * history may never have held it, and the archive holds the reactions the store also drops.
 */
static void on_delete_message(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();
    const uint32_t packet_id = action->number;

    if (packet_id == 0U) {
        return; /* nothing names this message; the sheet does not open on one */
    }

    const bool is_channel = (action->dest == MESH_MESSAGE_BROADCAST_ADDR);
    const uint8_t kind =
        is_channel ? (uint8_t)MESH_UI_CONVERSATION_CHANNEL : (uint8_t)MESH_UI_CONVERSATION_DIRECT;
    const uint32_t node = is_channel ? 0U : action->dest;
    const uint8_t channel = is_channel ? action->channel : 0U;

    /*
     * All four scoped to the conversation the press came from, not just to the packet id.
     *
     * The archive is scoped by construction - it opens that conversation's file and no other -
     * and the three in RAM have to be told, because a packet id is only unique per sender for a
     * few minutes (mesh_session_next_packet_id). An id on its own would let one press delete a
     * message in a conversation the user was never looking at, and the handshake cache would
     * then write that absence to the card.
     */
    const uint32_t ring_peer = is_channel ? MESH_MESSAGE_BROADCAST_ADDR : action->dest;
    (void)mesh_session_forget_message(&app->session, ring_peer, action->channel, packet_id);
    (void)mesh_ui_message_list_forget_message(&app->ui_messages_cached, kind, node, channel,
                                              packet_id);
    const int archived =
        mesh_ui_archive_forget_message(&app->ui_archive, kind, node, channel, packet_id);
    if (archived < 0) {
        inkwell_log_warn("ui", "Could not remove message %u from the stored transcript: %d",
                         packet_id, archived);
    }
    const uint32_t removed =
        mesh_ui_store_forget_message(&app->ui_store, kind, node, channel, packet_id);

    /* Written straight back out, so the message does not come back on the next start. */
    if (app->ui_handshake_cache_path[0] != '\0') {
        app->ui_handshake_cache_dirty = true;
        mesh_app_flush_ui_cache(app);
    }

    if (removed > 0U || archived > 0) {
        inkwell_str_copy(toast, sizeof toast, inkcell_str(MESH_STR_TOAST_MESSAGE_DELETED));
        inkwell_log_info("ui", "Deleted message %u", packet_id);
    } else {
        inkwell_str_copy(toast, sizeof toast, inkcell_str(MESH_STR_TOAST_MESSAGE_NOT_FOUND));
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_delete_conversation(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    /*
     * A message lives in three places at once, and the delete has to reach all three or the
     * next publish undoes it: the transport's ring, the history read back from the cache at
     * startup, and the store the backends draw. The ring is the one that matters most -
     * mesh_app_publish_messages() rebuilds the store from it on every frame that carries
     * traffic, so a store-only delete would survive about a second.
     */
    const bool is_channel = (action->number == (uint32_t)MESH_UI_CONVERSATION_CHANNEL);
    const uint32_t peer = is_channel ? MESH_MESSAGE_BROADCAST_ADDR : action->dest;
    /* The name the row was showing, carried on the action. */
    const char *name = action->text;

    (void)mesh_session_forget_conversation(&app->session, peer, action->channel);
    (void)mesh_ui_message_list_forget(&app->ui_messages_cached, (uint8_t)action->number,
                                      action->dest, action->channel);
    /* And the fourth place, which is the one that outlives the other three: the card's own
       transcript. A conversation deleted everywhere else and left on the card would come back
       in full the next time the reader opened it. */
    const int archived = mesh_ui_archive_forget_conversation(
        &app->ui_archive, (uint8_t)action->number, action->dest, action->channel);
    if (archived < 0) {
        inkwell_log_warn("ui", "Could not remove the stored transcript for %s: %d", name, archived);
    }
    const uint32_t removed = mesh_ui_store_forget_conversation(
        &app->ui_store, (uint8_t)action->number, action->dest, action->channel);

    /* Written straight back out, so the messages do not come back on the next start. */
    if (app->ui_handshake_cache_path[0] != '\0') {
        app->ui_handshake_cache_dirty = true;
        mesh_app_flush_ui_cache(app);
    }

    if (removed > 0U) {
        inkcell_str_format_plural(toast, sizeof toast, MESH_STR_TOAST_DELETED_MESSAGES_ONE, removed,
                                  (unsigned)removed, name);
        inkwell_log_info("ui", "Deleted %u message(s) in the conversation with %s",
                         (unsigned)removed, name);
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_NOTHING_TO_DELETE, name);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_mute_conversation(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    /*
     * Purely local: nothing goes on the air, and the only state that moves is the read mark
     * this conversation shares a slot with. It comes through an action all the same because
     * the nav is handed a `const` store and cannot write one - the delete above is here for
     * the same reason, and both raise the toast from the row's own name.
     *
     * The press is a bare toggle, so what it means is decided here, where both halves of
     * mesh_ui_store_conversation_muted() can be asked apart. A direct conversation whose
     * node the *radio* is muting has nothing a local unmute could achieve, so it is told
     * rather than silently doing nothing - the row would still draw itself muted, and a
     * press that appears to fail is worse than one that explains itself.
     */
    const uint8_t kind = (uint8_t)action->number;
    const char *name = action->text;
    const bool local = mesh_ui_store_conversation_muted_locally(&app->ui_store, kind, action->dest,
                                                                action->channel);
    const bool effective =
        mesh_ui_store_conversation_muted(&app->ui_store, kind, action->dest, action->channel);

    if (!local && effective) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_CONVO_MUTED_ON_RADIO, name);
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }

    (void)mesh_ui_store_set_conversation_mute(&app->ui_store, kind, action->dest, action->channel,
                                              !local);

    /*
     * And what the press actually achieved, asked again rather than assumed.
     *
     * Both halves can be on at once - mute a conversation here, then mute the same node from
     * the Nodes tab or from another client - and there the guard above does not fire,
     * because the local half really was on and really has just been cleared. What has not
     * changed is the row: the radio is still muting that node, so it draws itself muted and
     * goes on interrupting nobody. Saying "Unmuted" there is the one thing worse than the
     * press doing nothing, which is the press lying about it.
     */
    const bool still_muted =
        mesh_ui_store_conversation_muted(&app->ui_store, kind, action->dest, action->channel);
    if (local && still_muted) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_CONVO_MUTED_ON_RADIO, name);
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        inkwell_log_info("ui", "Cleared the local mute on %s; the radio still mutes it", name);
        return;
    }
    inkcell_str_format(toast, sizeof toast,
                       !local ? MESH_STR_TOAST_CONVO_MUTED : MESH_STR_TOAST_CONVO_UNMUTED, name);
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
    inkwell_log_info("ui", "%s the conversation with %s", !local ? "Muted" : "Unmuted", name);
}

static void on_traceroute(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    action_peer_name(app, action->dest, name, sizeof name);
    const int result = mesh_session_send_traceroute(&app->session, action->dest);
    if (result == 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_TRACING, name);
        inkwell_log_info("ui", "Traceroute to 0x%08x from the Nodes tab", action->dest);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else if (result == -EBUSY) {
        /* One trace at a time is this client's half of the firmware's rate limit. */
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_TRACE_RUNNING));
    } else if (result == -EINVAL) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_CANNOT_TRACE));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_TRACE_FAILED, result);
        inkwell_log_warn("ui", "Traceroute to 0x%08x failed: %d", action->dest, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_share_waypoint(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    /*
     * Two jobs behind one action, told apart by `number`: 0 is a new place at a node's fix,
     * anything else re-broadcasts a place we already hold.
     *
     * The coordinate is read here rather than carried through the nav on purpose. The
     * session roster is the authority - it holds 256 nodes where the published one holds
     * 128, and it is current rather than a snapshot from whenever the key was pressed - so
     * a node that moved while its name was being typed is saved where it actually is.
     */
    const struct mesh_handshake_status *status = mesh_session_handshake(&app->session);
    struct mesh_waypoint waypoint;
    memset(&waypoint, 0, sizeof waypoint);

    uint8_t channel = 0U;
    if (action->number != 0U) {
        const struct mesh_waypoint *existing =
            mesh_waypoint_book_get(mesh_session_waypoints(&app->session), action->number);
        if (existing == NULL) {
            snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_WAYPOINT_GONE));
            mesh_ui_store_set_toast(&app->ui_store, now, toast);
            return;
        }
        waypoint = *existing;
        channel = existing->channel;
    } else {
        if (action->text[0] == '\0') {
            snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_WAYPOINT_NAME_NEEDED));
            mesh_ui_store_set_toast(&app->ui_store, now, toast);
            return;
        }
        /* `dest` of 0 means our own radio, which is the "New waypoint here" row; anything
           else is the node a "Save this place" row was pressed on. */
        const uint32_t source = action->dest != 0U
                                    ? action->dest
                                    : (status->has_my_info ? status->my_info.my_node_num : 0U);
        const struct mesh_node_summary *node = NULL;
        for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
            if (source != 0U && status->nodes[i].node_id == source) {
                node = &status->nodes[i];
                break;
            }
        }
        if (node == NULL || !node->position.valid ||
            !mesh_geo_coords_valid(node->position.latitude_i, node->position.longitude_i)) {
            snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_WAYPOINT_NO_FIX));
            mesh_ui_store_set_toast(&app->ui_store, now, toast);
            return;
        }
        waypoint.has_coords = true;
        waypoint.latitude_i = node->position.latitude_i;
        waypoint.longitude_i = node->position.longitude_i;
        inkwell_str_copy(waypoint.name, sizeof waypoint.name, action->text);
        /*
         * Locked to us: we made it, and nobody else on the mesh has a reason to move it.
         * Left open, any client could edit or withdraw it - which upstream allows and which
         * is the wrong default for a place somebody deliberately marked.
         */
        waypoint.locked_to = status->has_my_info ? status->my_info.my_node_num : 0U;
        channel = mesh_app_primary_channel(status);
    }

    uint32_t id = 0U;
    const int result = mesh_session_send_waypoint(&app->session, &waypoint, channel, &id);
    if (result == 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_WAYPOINT_SHARED, waypoint.name);
        inkwell_log_info("ui", "Shared waypoint %u on channel %u", id, (unsigned)channel);
    } else if (result == -ENOTCONN) {
        /* The session kept the place regardless - see mesh_session_send_waypoint(). What
           did not happen is the mesh hearing about it, and that is what the toast says. */
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_WAYPOINT_SAVED, waypoint.name);
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_WAYPOINT_FAILED, result);
        inkwell_log_warn("ui", "Sharing a waypoint failed: %d", result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
    mesh_app_publish_ui_state(app);
}

static void on_forget_waypoint(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    const struct mesh_waypoint *existing =
        mesh_waypoint_book_get(mesh_session_waypoints(&app->session), action->number);
    char name[MESH_WAYPOINT_NAME_MAX + 1U];
    inkwell_str_copy(name, sizeof name,
                     (existing != NULL && existing->name[0] != '\0')
                         ? existing->name
                         : inkcell_str(MESH_STR_WAYPOINTS_UNNAMED));

    bool shared = false;
    const int result = mesh_session_forget_waypoint(&app->session, action->number, &shared);
    if (result == -ENOENT) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_WAYPOINT_GONE));
    } else if (shared) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_WAYPOINT_DELETED, name);
    } else {
        /* The place is gone from here either way; the toast says whether the mesh heard
           about it, because a locked place and a dead link both end up here. */
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_WAYPOINT_FORGOT, name);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
    mesh_app_publish_ui_state(app);
}

static void on_disconnect(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    struct mesh_transport *transport = mesh_app_active_transport();
    const char *identifier = mesh_app_connected_identifier();
    char name[64];
    snprintf(name, sizeof name, "%s",
             action->identifier[0] != '\0' ? action->identifier
                                           : (identifier != NULL ? identifier : ""));

    /*
     * Every transport gets its own arm, and the last one is not an "everything else".
     * mesh_app_active_transport() can name any of the three, and each of these casts
     * transport->state to its own struct - so a link routed to the wrong one is not a
     * disconnect that fails, it is a read of one transport's state through another's type.
     */
    int result = -ENOTCONN;
    if (transport == mesh_serial_transport()) {
        result = mesh_serial_transport_disconnect(transport);
    } else if (transport == mesh_tcp_transport()) {
        result = mesh_tcp_transport_disconnect(transport);
    } else if (transport == mesh_ble_transport()) {
        result = mesh_ble_transport_disconnect(transport);
    }

    if (result == 0) {
        /* Holding auto-connect is the point of the press: without it the next loop turn
           takes the same radio straight back. */
        app->autoconnect_held = true;
        app->ui_report_link_error = false;
        if (name[0] != '\0') {
            inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_DISCONNECTED_FROM, name);
        } else {
            snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_DISCONNECTED));
        }
        inkwell_log_info("ui", "Disconnect requested from the device (%s)",
                         name[0] != '\0' ? name : "active link");
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOTHING_CONNECTED));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_DISCONNECT_FAILED, result);
        inkwell_log_warn("ui", "Disconnect failed: %d", result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_forget(struct mesh_app *app, const struct mesh_ui_action *action) {
    struct mesh_transport *const ble = mesh_ble_transport();
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    if (action->kind == (uint8_t)MESH_UI_DEVICE_TCP) {
        /*
         * Clearing the network address, which the Devices tab raises by finishing its
         * keyboard with an empty field. Three places hold it and all three have to let go
         * or auto-connect goes on reaching for a host with no row on any screen: the
         * transport, which is what auto-connect asks; `config`, which seeds the transport
         * on the next start; and the preferences file, which seeds `config`.
         */
        if (app->config.preferred_tcp_host[0] == '\0' &&
            app->ui_preferences.network_host[0] == '\0') {
            mesh_ui_store_set_toast(&app->ui_store, now,
                                    inkcell_str(MESH_STR_TOAST_NO_NETWORK_HOST));
            return;
        }
        (void)mesh_tcp_transport_forget(mesh_tcp_transport());
        app->config.preferred_tcp_host[0] = '\0';
        app->ui_preferences.network_host[0] = '\0';
        app->ui_preferences_dirty = true;
        /* A host we have just thrown away is not one to reconnect to on the next tick. */
        app->autoconnect_tcp_retry_at_ms = 0U;
        inkwell_log_info("ui", "Forgot the network address");
        mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_FORGOT_NETWORK));
        mesh_app_publish_ui_state(app);
        return;
    }
    if (ble == NULL || action->kind != (uint8_t)MESH_UI_DEVICE_BLE) {
        mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_BLE_ONLY_PAIRING));
        return;
    }
    const bool was_connected = (mesh_app_connected_identifier() != NULL &&
                                strcmp(mesh_app_connected_identifier(), action->identifier) == 0);
    const int result = mesh_ble_transport_forget(ble, action->identifier);
    if (result == 0) {
        /* The bond is gone, so a reconnect would only fail on StartNotify until the user
           pairs again; do not let auto-connect spend the next minute proving it. */
        if (was_connected) {
            app->autoconnect_held = true;
        }
        /* And drop it from the radios we reach for. A node whose bond we just threw away
           is the one thing auto-connect must not rank first the next time it is in the
           room; the list falls through to the radio used before it. */
        if (mesh_ui_preferences_forget_device(&app->ui_preferences, action->identifier,
                                              action->kind)) {
            app->ui_preferences_dirty = true;
        }
        if (strcasecmp(app->config.preferred_ble_device, action->identifier) == 0) {
            snprintf(app->config.preferred_ble_device, sizeof app->config.preferred_ble_device,
                     "%s",
                     app->ui_preferences.preferred_device_kind == (uint8_t)MESH_UI_DEVICE_SERIAL
                         ? ""
                         : app->ui_preferences.preferred_device);
        }
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_FORGOT_DEVICE, action->identifier);
        inkwell_log_info("ui", "Forgot BLE node %s", action->identifier);
    } else if (mesh_transport_registry_take_error(&app->transport_registry, toast, sizeof toast)) {
        inkwell_log_warn("ui", "Forget %s failed: %s (%d)", action->identifier, toast, result);
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_FORGET_DEVICE_FAILED, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_submit_passkey(struct mesh_app *app, const struct mesh_ui_action *action) {
    /* Non-NULL: the table's needs_ble is what asked. */
    struct mesh_transport *const ble = mesh_ble_transport();
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    const unsigned long value = strtoul(action->text, NULL, 10);
    const int result = mesh_ble_transport_submit_passkey(ble, (uint32_t)value);
    if (result == 0) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_PAIRING));
        /* Whatever goes wrong from here is reported by the transport, not by this call. */
        app->ui_report_link_error = true;
    } else if (result == -ENOENT) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_PAIRING_EXPIRED));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_PAIRING_FAILED, result);
        inkwell_log_warn("ui", "Passkey submit failed: %d", result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_cancel_pairing(struct mesh_app *app, const struct mesh_ui_action *action) {
    struct mesh_transport *const ble = mesh_ble_transport();
    const uint64_t now = inkwell_time_monotonic_ms();
    (void)action;

    if (ble != NULL) {
        (void)mesh_ble_transport_cancel_pairing(ble);
    }
    /* A cancelled pairing is a cancelled connect: do not let auto-connect start it over. */
    app->autoconnect_held = true;
    app->ui_report_link_error = false;
    mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_PAIRING_CANCELLED));
}

static void on_add_contact(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    action_peer_name(app, action->dest, name, sizeof name);
    const int result = mesh_session_add_contact(&app->session, action->dest);
    if (result > 0) {
        /* "Sent", not "added": the radio's database may be full, and what settles whether
           the entry landed is this node's next NodeInfo rather than the ack for this
           request. Claiming the row early would promise an encrypted direct message that
           may still have nothing to encrypt with. */
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_CONTACT_SENT, name);
        inkwell_log_info("ui", "Added node 0x%08x to the NodeDB from the Nodes tab", action->dest);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else if (result == -ENOENT) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NODE_GONE));
    } else if (result == -EINVAL) {
        /* The one thing this verb cannot do without, said as the reason rather than as a
           refusal: an entry with no key is what the radio would build for itself. */
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_CONTACT_NO_KEY, name);
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_CONTACT_FAILED, result);
        inkwell_log_warn("ui", "Add contact for 0x%08x failed: %d", action->dest, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_verify_key(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    action_peer_name(app, action->dest, name, sizeof name);
    const int result = mesh_session_verify_key_begin(&app->session, action->dest);
    if (result > 0) {
        /* Said as what happens next rather than as "started": the two radios have to reach
           each other before anything is asked of anybody, and on a mesh that is seconds at
           best. The sheet arrives on its own when there is something to answer. */
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_VERIFY_STARTED, name);
        /* A press is always a new question. A second press on an exchange that is still waiting
           starts over in the stage the sheet last showed, and the publish opens the sheet only
           on a stage it has not shown - so a waiting sheet dismissed with B never came back. */
        app->ui_verify_stage_shown = (uint8_t)MESH_KEY_VERIFICATION_IDLE;
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else if (result == -ENOENT) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NODE_GONE));
    } else if (result == -EINVAL) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_VERIFY_NO_KEY));
    } else if (result == -EBUSY) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_VERIFY_BUSY));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_VERIFY_FAILED, result);
        inkwell_log_warn("ui", "Key verification with 0x%08x failed to start: %d", action->dest,
                         result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_verify_number(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    const unsigned long value = strtoul(action->text, NULL, 10);
    const int result = mesh_session_verify_key_number(&app->session, (uint32_t)value);
    if (result >= 0) {
        /* Nothing to announce: the radios now finish the handshake and the sheet comes
           back with the characters to compare, which is a better answer than a toast. */
        return;
    }
    if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_VERIFY_FAILED, result);
        inkwell_log_warn("ui", "Security number rejected: %d", result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_verify_answer(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    const bool verified = (action->number != 0U);
    /* The name is read before the answer, because answering ends the exchange and takes
       the name with it. */
    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    const struct mesh_key_verification *const live = mesh_session_verification(&app->session);
    (void)inkwell_str_copy(name, sizeof name,
                           live->remote_name[0] != '\0' ? live->remote_name
                                                        : inkcell_str(MESH_STR_COMMON_UNKNOWN));
    const int result = mesh_session_verify_key_settle(&app->session, verified);
    if (result < 0 && result != -ENOTCONN) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_VERIFY_FAILED, result);
        inkwell_log_warn("ui", "Key verification answer rejected: %d", result);
    } else if (result == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else {
        inkcell_str_format(toast, sizeof toast,
                           verified ? MESH_STR_TOAST_VERIFY_DONE : MESH_STR_TOAST_VERIFY_REFUSED,
                           name);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_cycle_update_channel(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();
    (void)action;

    /* Steps DEFAULT -> STABLE -> PRERELEASE -> DEFAULT. Saved immediately rather than
       collected as a pending edit: About has no Y-save, because there is no radio write
       behind it. */
    const enum mesh_update_channel next = (enum mesh_update_channel)(
        ((unsigned)app->updater.channel + 1U) % (unsigned)MESH_UPDATE_CHANNEL_COUNT);
    if (!mesh_updater_set_channel(&app->updater, next)) {
        mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_BUSY_RETRY));
        return;
    }
    app->ui_preferences.update_channel = (uint8_t)app->updater.channel;
    app->ui_preferences_dirty = true;
    inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_UPDATE_CHANNEL,
                       (int)(sizeof toast - 18U), mesh_update_channel_name(app->updater.channel));
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_cycle_language(struct mesh_app *app, const struct mesh_ui_action *action) {
    const uint64_t now = inkwell_time_monotonic_ms();
    (void)action;

    if (inkcell_i18n_is_overridden()) {
        return;
    }
    const size_t count = inkcell_i18n_locale_count();
    for (size_t i = 0; i < count; ++i) {
        if (inkcell_i18n_locale_at(i) != inkcell_i18n_locale()) {
            continue;
        }
        const struct inkcell_i18n_locale *next = inkcell_i18n_locale_at((i + 1U) % count);
        (void)inkcell_i18n_set_locale(next->id);
        inkwell_str_copy(app->ui_preferences.language, sizeof app->ui_preferences.language,
                         next->id);
        app->ui_preferences_dirty = true;
        mesh_ui_store_set_toast(&app->ui_store, now, next->name);
        /* Persist and rebuild formatted snapshot text before the queued redraw. */
        mesh_app_publish_ui_state(app);
        break;
    }
}

static void on_cycle_theme(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();
    (void)action;

    /* Saved immediately rather than collected as a pending edit, for the reason the update
       channel is: About has no Y-save, because there is no radio write behind it.
       The frame after this one is drawn in the new theme - the backends read it out of the
       client info in the snapshot - so the press is its own confirmation. */
    if (app->ui_theme_from_env) {
        mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_THEME_HELD));
        return;
    }
    const struct inkcell_theme *next = inkcell_theme_next(app->ui_theme);
    if (next == NULL) {
        return;
    }
    app->ui_theme = next;
    inkwell_str_copy(app->ui_preferences.theme, sizeof app->ui_preferences.theme, next->id);
    app->ui_preferences_dirty = true;
    inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_THEME, (int)(sizeof toast - 8U),
                       next->name);
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
    /*
     * Published here rather than left to the next loop turn. This handler runs inside
     * inkwell_loop_run(), and the toast above has already queued a redraw that the same
     * turn will drain - so without this the press's own frame would arrive with the new
     * toast drawn in the old theme, and the switch would land a turn later. That gap is
     * exactly what "the frame the press draws is the answer" is not.
     */
    mesh_app_publish_ui_state(app);
}

static void on_toggle_dev_updates(struct mesh_app *app, const struct mesh_ui_action *action) {
    const uint64_t now = inkwell_time_monotonic_ms();
    (void)action;

    if (!mesh_updater_set_allow_dev(&app->updater, !app->updater.allow_dev)) {
        mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_BUSY_RETRY));
        return;
    }
    app->ui_preferences.update_allow_dev = app->updater.allow_dev;
    app->ui_preferences_dirty = true;
    mesh_ui_store_set_toast(&app->ui_store, now,
                            inkcell_str(app->updater.allow_dev ? MESH_STR_TOAST_DEV_UPDATES_ON
                                                               : MESH_STR_TOAST_DEV_UPDATES_OFF));
}

static void on_discard_crash_report(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();
    (void)action;

    /*
     * Local, immediate and its own confirmation, like the theme above: About has no Y-save
     * because there is no radio write behind any of it.
     *
     * No confirm dialog either. The five questions that get one all take something away
     * that cannot be had back - a radio's config, a node the mesh may not mention again -
     * and this takes away a copy of a file the user has already been told where to find.
     * A dialog here would be spending a press to protect a diagnostic.
     *
     * The publish is what clears the banner and the two rows in the same frame: both read
     * `crash_report_waiting`, which is re-read from the module rather than assumed, so a
     * discard that somehow failed leaves the notice standing rather than hiding a report
     * that is still on the card.
     */
    const int result = inkwell_crash_discard();
    if (result < 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_CRASH_DISCARD_FAILED, -result);
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_CRASH_DISCARDED));
    mesh_app_publish_ui_state(app);
}

static void on_check_update(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();
    (void)action;

    const int result = mesh_updater_check(&app->updater, now);
    if (result == 0) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_CHECKING_UPDATES));
    } else if (result == -ENOTSUP) {
        inkwell_str_copy(toast, sizeof toast,
                         app->updater.message[0] != '\0'
                             ? app->updater.message
                             : inkcell_str(MESH_STR_TOAST_UPDATES_UNAVAILABLE));
    } else if (result == -EBUSY) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_ALREADY_CHECKING));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_UPDATE_CHECK_FAILED, result);
        inkwell_log_warn("ui", "Update check could not start: %d", result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_cycle_firmware_channel(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();
    (void)action;

    const enum mesh_firmware_channel next = (enum mesh_firmware_channel)(
        ((unsigned)app->firmware.channel + 1U) % (unsigned)MESH_FIRMWARE_CHANNEL_COUNT);
    if (!mesh_firmware_set_channel(&app->firmware, next)) {
        /* Refused, which here only ever means a check is in flight. */
        mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_ALREADY_CHECKING));
        return;
    }
    app->ui_preferences.firmware_channel = (uint8_t)app->firmware.channel;
    app->ui_preferences_dirty = true;
    inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_FIRMWARE_CHANNEL,
                       mesh_firmware_channel_name(app->firmware.channel));
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_check_radio_firmware(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();
    (void)action;

    /* What the radio said about itself is the whole input: the model number decides which
       board this is and the version decides whether the newest release is news. Both may
       be absent, and a check on either still reports what upstream has published - see
       mesh_firmware_check().
       The link's copy, because the image this leads to is written to the radio on the end of
       the link. While the Settings tab is administering another node it is *that* node's model
       in `metadata`, and checking against it would offer an image for a board nobody here is
       holding. */
    const meshtastic_DeviceMetadata *const metadata =
        mesh_radio_settings_link_metadata(mesh_session_settings(&app->session));
    const int result =
        mesh_firmware_check(&app->firmware, metadata != NULL ? (uint32_t)metadata->hw_model : 0U,
                            metadata != NULL ? metadata->firmware_version : "", now);
    if (result == 0) {
        inkwell_str_copy(toast, sizeof toast, inkcell_str(MESH_STR_TOAST_CHECKING_FIRMWARE));
    } else if (result == -ENOTSUP) {
        inkwell_str_copy(toast, sizeof toast, inkcell_str(MESH_STR_TOAST_UPDATES_UNAVAILABLE));
    } else if (result == -EBUSY) {
        inkwell_str_copy(toast, sizeof toast, inkcell_str(MESH_STR_TOAST_ALREADY_CHECKING));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_UPDATE_CHECK_FAILED, result);
        inkwell_log_warn("ui", "Firmware check could not start: %d", result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static void on_install_update(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();
    (void)action;

    const int result = mesh_updater_install(&app->updater, now);
    if (result == 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_DOWNLOADING, app->updater.latest);
        inkwell_log_info("ui", "Installing update %s from the About screen", app->updater.latest);
    } else if (result == -EBUSY) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_ALREADY_WORKING));
    } else if (result == -EINVAL) {
        /* Nothing to install: the check has not run, or found nothing newer. */
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_CHECK_FIRST));
    } else if (result == -ENOTSUP) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_UPDATES_UNAVAILABLE));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_UPDATE_FAILED, result);
        inkwell_log_warn("ui", "Update install could not start: %d", result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

/*
 * Going back to a radio already in its loader, which is the press the banner promises. True
 * when the press was spent on it, so the install below knows to stand down.
 *
 * It is answered before everything the install does because none of it applies: there is no
 * link, so no bus and no board to identify, and the check's answer was dropped when the radio
 * stopped answering. What stands in for all of it is the job's own memory of what it was doing
 * - and the hooks lose their two radio-facing halves, because a loader is not a radio: nothing
 * is armed (firmware_ota.c takes a NULL arm as "already in there") and nothing is waited for (a
 * NULL radio_ready is "go now").
 */
static bool firmware_resume_install(struct mesh_app *app, uint64_t now) {
    if (!mesh_firmware_update_can_resume(&app->firmware_update)) {
        return false;
    }
    char toast[MESH_UI_NAV_TOAST_MAX];
    const struct mesh_firmware_board board = app->firmware_update.board;
    const struct mesh_firmware_release release = app->firmware_update.release;
    char where[sizeof app->firmware_update.where];
    inkwell_str_copy(where, sizeof where, app->firmware_update.where);
    const struct mesh_firmware_update_hooks resume = {
        .release_link = mesh_app_firmware_release_link,
        .request_interval = mesh_app_firmware_interval,
        .userdata = app,
    };
    const int resumed = mesh_firmware_update_start(&app->firmware_update, &board, &release, where,
                                                   &resume, mesh_app_firmware_update_done, app);
    if (resumed == 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_INSTALLING_FIRMWARE,
                           release.version);
        inkwell_log_info("ui", "Resuming the firmware install: %s is in its OTA loader",
                         where[0] != '\0' ? where : board.target);
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_FIRMWARE_FAILED,
                           mesh_firmware_update_error_name(app->firmware_update.error));
        inkwell_log_warn("ui", "Firmware install could not resume: %d", resumed);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
    return true;
}

static void on_install_radio_firmware(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    if (firmware_resume_install(app, now)) {
        return;
    }
    /*
     * The board the check identified, never one of several candidates: mesh_firmware_board()
     * answers NULL when the model is claimed by more than one target, and flashing the wrong
     * variant of the right board is the failure this feature must not have.
     */
    const struct mesh_firmware_board *const board = mesh_firmware_board(&app->firmware);
    if (board == NULL || app->firmware.state != MESH_FIRMWARE_AVAILABLE) {
        mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_CHECK_FIRST));
        return;
    }
    /*
     * The bus the sheet named, against the bus this would actually use.
     *
     * `number` is what the user agreed to and the other two are what is true now, and the
     * whole reason the sheet carries a bus at all is that the two warnings are different
     * sentences: one is a cable that has to stay in and the other is a radio leaving the
     * mesh. A link that changed between the question and the answer - a cable pulled while
     * the sheet was up, an auto-connect landing on the other transport - would otherwise
     * have somebody agree to the mild warning and get the severe one. Refused rather than
     * re-asked: the row is still there, and it will be offering the right sheet.
     */
    const enum mesh_firmware_path agreed =
        action->number == 1U ? MESH_FIRMWARE_PATH_BLE : MESH_FIRMWARE_PATH_USB;
    if (board->path != agreed || app->firmware.bus != agreed) {
        mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_CHECK_FIRST));
        inkwell_log_warn("ui",
                         "Refusing a firmware install: the sheet said %s and the radio is "
                         "on %s",
                         agreed == MESH_FIRMWARE_PATH_BLE ? "BLE" : "USB",
                         app->firmware.bus == MESH_FIRMWARE_PATH_BLE ? "BLE" : "USB");
        return;
    }
    /*
     * Where the radio is, in whichever terms its own bus uses - a serial port id, or a BLE
     * address. It is what the handover watches for the radio coming back on, and on the USB
     * path it is also what stops a write following a board that moved to another port.
     *
     * Read now rather than when the handover starts, because the download takes the link
     * down: by the time the image has landed there is nothing to ask.
     */
    const char *where = mesh_app_connected_identifier();
    if (agreed == MESH_FIRMWARE_PATH_USB) {
        /*
         * The transport's own id, not the identifier the device row carries. That field is a
         * *label* - the tty when there is one - and the bootloader is looked for on the USB
         * device, so a label makes the handover wait out its timeout and report that no
         * bootloader came. The same correction phase 3's CLI path already carries.
         */
        where = mesh_serial_transport_connected_id(mesh_serial_transport());
    }
    /*
     * Where the radio's own notices have got to, so this install starts from now.
     *
     * Without it, a "Rebooting to BLE OTA" left over from an earlier install - or from
     * another client on the same radio - is the first thing the relay below hands over, and
     * arming believes it and moves on before the radio has heard anything.
     */
    const struct mesh_client_notification *const seen = mesh_session_notification(&app->session);
    app->firmware_notification_seq = seen != NULL ? seen->seq : 0U;
    const struct mesh_firmware_update_hooks hooks = mesh_app_firmware_hooks(app);
    const int result = mesh_firmware_update_start(
        &app->firmware_update, board, &app->firmware.release, where != NULL ? where : "", &hooks,
        mesh_app_firmware_update_done, app);
    if (result == 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_INSTALLING_FIRMWARE,
                           app->firmware.release.version);
        inkwell_log_info("ui", "Installing radio firmware %s on %s", app->firmware.release.version,
                         board->target);
    } else if (result == -EBUSY) {
        inkwell_str_copy(toast, sizeof toast, inkcell_str(MESH_STR_TOAST_ALREADY_WORKING));
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_FIRMWARE_FAILED,
                           mesh_firmware_update_error_name(app->firmware_update.error));
        inkwell_log_warn("ui", "Radio firmware install could not start: %d", result);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

/* ---- the table ------------------------------------------------------------------------------ */

/*
 * What each press runs, one row per verb.
 *
 * A table for the reason the ones in src/ui/tables/actions.c, status.c and help.c are: what an
 * action does should be readable in one place and checkable against the nav that raises it. The
 * order is the order the verbs are declared in mesh/ui/nav.h, so a new one goes where its
 * enumerator is rather than wherever the last was appended.
 *
 * `needs_ble` is the one condition four of them share - no adapter means nothing to ask - and it
 * is a column rather than the first four lines of each of those arms. Everything else a press
 * needs to be true is its own handler's business, the way a status row states its own condition
 * and nothing else's.
 */
struct app_action_entry {
    enum mesh_ui_action_type type;
    void (*run)(struct mesh_app *app, const struct mesh_ui_action *action);
    bool needs_ble;
};

/*
 * A Meshtastic channel link, joined.
 *
 * The link is parsed here rather than in the nav, against the radio's table *as it stands now*:
 * the sheet the user answered may have been up while a read-back landed, and what gets
 * overwritten is whatever is on the radio at this moment. Parsing twice is the cheap half of
 * that; the nav's parse decided whether to raise the sheet at all.
 */
static void on_import_channels(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    meshtastic_ChannelSet set;
    bool add = false;
    if (!mesh_channel_url_decode(action->text, &set, &add)) {
        mesh_ui_store_set_toast(&app->ui_store, now, inkcell_str(MESH_STR_TOAST_IMPORT_NOT_A_LINK));
        return;
    }

    const int queued = mesh_session_import_channels(&app->session, &set, add);
    if (queued > 0) {
        /*
         * Announced like a section save, because that is what it is: several channel writes and
         * a LoRa config, acked one at a time, with a reboot at the end. Tracking it through the
         * same counters is what makes the Settings tab say "saving" while they are in flight
         * and report the first one that the radio refuses.
         */
        const struct mesh_radio_settings *radio = mesh_session_settings(&app->session);
        app->settings_save_pending = true;
        app->settings_writes_acked_seen = radio != NULL ? radio->writes_acked : 0U;
        app->settings_writes_failed_seen = radio != NULL ? radio->writes_failed : 0U;
        app->settings_reboot_notices_seen = app->session.reboot_notices;
        app->settings_save_started_ms = now;
        snprintf(app->settings_save_section, sizeof app->settings_save_section, "%s",
                 inkcell_str(MESH_STR_SETTINGS_SECTION_CHANNELS));
        inkcell_str_format_plural(toast, sizeof toast, MESH_STR_TOAST_IMPORT_QUEUED_ONE,
                                  (uint32_t)set.settings_count, (unsigned)set.settings_count);
    } else if (queued == 0) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_IMPORT_NO_CHANGE));
    } else if (queued == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_IMPORT_FAILED));
        inkwell_log_warn("ui", "Channel import failed: %d", queued);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

/*
 * A Meshtastic contact link, added to the radio's NodeDB.
 *
 * on_import_channels()'s shape, and the link is parsed here rather than in the nav for its
 * reason: the nav's parse decided whether to raise the sheet at all, and this one runs against
 * the session as it stands now, which is what will carry the write.
 *
 * The one case worth its own sentence is a link naming this radio - somebody reading our own
 * contact code back in to see what happens, which is the likeliest way to arrive at a refusal
 * here. It is asked before the session is, so that the answer can say which of the two -EINVALs
 * it was: the session refuses our own node and a contact with no key with the same code, and
 * the decoder above has already ruled the second out.
 */
static void on_import_contact(struct mesh_app *app, const struct mesh_ui_action *action) {
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = inkwell_time_monotonic_ms();

    meshtastic_SharedContact contact;
    if (!mesh_contact_url_decode(action->text, &contact)) {
        mesh_ui_store_set_toast(&app->ui_store, now,
                                inkcell_str(MESH_STR_TOAST_CONTACT_LINK_INVALID));
        return;
    }

    const struct mesh_handshake_status *const status = mesh_session_handshake(&app->session);
    if (status != NULL && status->has_my_info && contact.node_num == status->my_info.my_node_num) {
        mesh_ui_store_set_toast(&app->ui_store, now,
                                inkcell_str(MESH_STR_TOAST_CONTACT_LINK_IS_SELF));
        return;
    }

    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    if (!mesh_ui_contact_link_name(action->text, name, sizeof name)) {
        name[0] = '\0';
    }
    const int queued = mesh_session_import_contact(&app->session, &contact);
    if (queued > 0) {
        /* "Sent", not "added", for the reason on_add_contact() gives: the radio's database may
           be full, and what settles whether the entry landed is this node's next NodeInfo
           rather than the ack for this request. */
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_CONTACT_LINK_QUEUED, name);
        inkwell_log_info("ui", "Added node 0x%08x to the NodeDB from a contact link",
                         (unsigned)contact.node_num);
    } else if (queued == -ENOTCONN) {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_NOT_CONNECTED));
    } else {
        snprintf(toast, sizeof toast, "%s", inkcell_str(MESH_STR_TOAST_CONTACT_LINK_FAILED));
        inkwell_log_warn("ui", "Contact import failed: %d", queued);
    }
    mesh_ui_store_set_toast(&app->ui_store, now, toast);
}

static const struct app_action_entry k_app_actions[] = {
    {MESH_UI_ACTION_CONNECT, on_connect, false},
    {MESH_UI_ACTION_SEND_TEXT, on_send_text, true},
    {MESH_UI_ACTION_RESEND, on_resend, true},
    {MESH_UI_ACTION_REFRESH_SETTINGS, on_refresh_settings, true},
    {MESH_UI_ACTION_SAVE_SETTINGS, on_save_settings, true},
    {MESH_UI_ACTION_RADIO_ACTION, on_radio_action, false},
    {MESH_UI_ACTION_FORGET_NODES, on_forget_nodes, false},
    {MESH_UI_ACTION_TOGGLE_FAVORITE, on_toggle_favorite, false},
    {MESH_UI_ACTION_REQUEST_NODE_INFO, on_request_node_info, false},
    {MESH_UI_ACTION_REQUEST_POSITION, on_request_reading, false},
    {MESH_UI_ACTION_REQUEST_TELEMETRY, on_request_reading, false},
    {MESH_UI_ACTION_TOGGLE_IGNORE, on_toggle_ignore, false},
    {MESH_UI_ACTION_TOGGLE_MUTE, on_toggle_mute, false},
    {MESH_UI_ACTION_REMOVE_NODE, on_remove_node, false},
    {MESH_UI_ACTION_DELETE_CONVERSATION, on_delete_conversation, false},
    {MESH_UI_ACTION_DELETE_MESSAGE, on_delete_message, false},
    {MESH_UI_ACTION_MUTE_CONVERSATION, on_mute_conversation, false},
    {MESH_UI_ACTION_TRACEROUTE, on_traceroute, false},
    {MESH_UI_ACTION_SHARE_WAYPOINT, on_share_waypoint, false},
    {MESH_UI_ACTION_FORGET_WAYPOINT, on_forget_waypoint, false},
    {MESH_UI_ACTION_DISCONNECT, on_disconnect, false},
    {MESH_UI_ACTION_FORGET, on_forget, false},
    {MESH_UI_ACTION_SUBMIT_PASSKEY, on_submit_passkey, true},
    {MESH_UI_ACTION_CANCEL_PAIRING, on_cancel_pairing, false},
    {MESH_UI_ACTION_ADD_CONTACT, on_add_contact, false},
    {MESH_UI_ACTION_VERIFY_KEY, on_verify_key, false},
    {MESH_UI_ACTION_VERIFY_NUMBER, on_verify_number, false},
    {MESH_UI_ACTION_VERIFY_ANSWER, on_verify_answer, false},
    {MESH_UI_ACTION_CYCLE_UPDATE_CHANNEL, on_cycle_update_channel, false},
    {MESH_UI_ACTION_CYCLE_LANGUAGE, on_cycle_language, false},
    {MESH_UI_ACTION_CYCLE_THEME, on_cycle_theme, false},
    {MESH_UI_ACTION_TOGGLE_DEV_UPDATES, on_toggle_dev_updates, false},
    {MESH_UI_ACTION_DISCARD_CRASH_REPORT, on_discard_crash_report, false},
    {MESH_UI_ACTION_CHECK_UPDATE, on_check_update, false},
    {MESH_UI_ACTION_CYCLE_FIRMWARE_CHANNEL, on_cycle_firmware_channel, false},
    {MESH_UI_ACTION_CHECK_RADIO_FIRMWARE, on_check_radio_firmware, false},
    {MESH_UI_ACTION_INSTALL_UPDATE, on_install_update, false},
    {MESH_UI_ACTION_INSTALL_RADIO_FIRMWARE, on_install_radio_firmware, false},
    {MESH_UI_ACTION_IMPORT_CHANNELS, on_import_channels, false},
    {MESH_UI_ACTION_IMPORT_CONTACT, on_import_contact, false},
    {MESH_UI_ACTION_SET_ADMIN_TARGET, on_set_admin_target, false},
};

/*
 * Every verb but MESH_UI_ACTION_NONE has a row.
 *
 * This is the one thing the table gives up against the switch it replaced. A `case` that was
 * never written is at least a gap somebody scrolling past can see; a missing row is a press
 * that arrives, matches nothing and returns, and the screen simply does not answer. Counted
 * rather than checked verb by verb, which is enough to catch the mistake that actually happens
 * - a row masking another's absence would mean writing the duplicate on purpose.
 */
_Static_assert(sizeof k_app_actions / sizeof k_app_actions[0] == (size_t)MESH_UI_ACTION_COUNT - 1U,
               "every mesh_ui_action_type but NONE needs a row in k_app_actions");

/* NULL for MESH_UI_ACTION_NONE and for anything the nav should never have sent, which is the
   `default: return;` the switch ended with. */
static const struct app_action_entry *app_action_entry_for(enum mesh_ui_action_type type) {
    for (size_t i = 0; i < sizeof k_app_actions / sizeof k_app_actions[0]; ++i) {
        if (k_app_actions[i].type == type) {
            return &k_app_actions[i];
        }
    }
    return NULL;
}

void mesh_app_on_ui_action(void *userdata, const struct mesh_ui_action *action) {
    struct mesh_app *const app = (struct mesh_app *)userdata;
    if (app == NULL || action == NULL) {
        return;
    }
    const struct app_action_entry *const entry = app_action_entry_for(action->type);
    if (entry == NULL) {
        return;
    }
    if (entry->needs_ble && mesh_ble_transport() == NULL) {
        mesh_ui_store_set_toast(&app->ui_store, inkwell_time_monotonic_ms(),
                                inkcell_str(MESH_STR_TOAST_BLE_UNAVAILABLE));
        return;
    }
    entry->run(app, action);
}
