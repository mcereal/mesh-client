#define _POSIX_C_SOURCE 200809L

/*
 * What the navigation model cannot do by itself: talk to the radio.
 *
 * One switch over mesh_ui_action_kind, reached from the UI controller. Everything it needs from
 * the rest of the app - connecting a link, queueing a settings write, naming a peer - is in
 * app_internal.h, so this file stays a dispatch table rather than growing its own logic.
 */

#include "app_internal.h"

#include "mesh/core/version.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/serial.h"
#include "mesh/ui/node_detail.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mesh_app_on_ui_action(void *userdata, const struct mesh_ui_action *action) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL || action == NULL) {
        return;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = mesh_time_monotonic_ms();

    switch (action->type) {
    case MESH_UI_ACTION_CONNECT: {
        mesh_log_info("ui", "Connect to %s (%s) requested from the device", action->identifier,
                      action->kind == (uint8_t)MESH_UI_DEVICE_SERIAL ? "usb" : "ble");
        snprintf(app->ui_preferences.preferred_device, sizeof app->ui_preferences.preferred_device,
                 "%s", action->identifier);
        app->ui_preferences.preferred_device_kind = action->kind;
        app->ui_preferences_dirty = true;
        /* Asking for a radio lifts a hold an earlier disconnect put on auto-connect. */
        app->autoconnect_held = false;
        app->autoconnect_failures = 0U;
        app->autoconnect_retry_at_ms = 0U;

        /* A user pick beats whatever auto-connect is doing or has done, on either link. */
        const int result = mesh_app_link_connect(app, action->identifier, action->kind);
        if (result == 0 || result == -EALREADY || result == -EINPROGRESS) {
            snprintf(toast, sizeof toast, "Connecting to %.40s", action->identifier);
            /* BLE resolves services from tick(), so a 0 here is not yet a connection. Arm the
               error report so whatever goes wrong next reaches the screen. */
            app->ui_report_link_error = true;
        } else if (mesh_transport_registry_take_error(&app->transport_registry, toast,
                                                      sizeof toast)) {
            mesh_log_warn("ui", "Connect to %s failed: %s (%d)", action->identifier, toast, result);
        } else {
            snprintf(toast, sizeof toast, "Connect failed (%d)", result);
            mesh_log_warn("ui", "Connect to %s failed: %d", action->identifier, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_SEND_TEXT: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, "BLE transport unavailable");
            return;
        }
        const bool broadcast = (action->dest == MESH_MESSAGE_BROADCAST_ADDR);
        uint32_t packet_id = 0U;
        const int result = mesh_session_send_text(&app->session, action->dest, action->channel,
                                                  action->text, !broadcast, &packet_id);
        if (result == 0) {
            snprintf(toast, sizeof toast, "Sent to %s", app->ui_store.nav.target_name);
            mesh_log_info("ui", "Sent \"%s\" to %s (packet %u)", action->text,
                          app->ui_store.nav.target_name, packet_id);
            mesh_app_watch_sent(app, packet_id, app->ui_store.nav.target_name);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else {
            snprintf(toast, sizeof toast, "Send failed (%d)", result);
            mesh_log_warn("ui", "Send to %s failed: %d", app->ui_store.nav.target_name, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_REFRESH_SETTINGS: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, "BLE transport unavailable");
            return;
        }
        const int result = mesh_session_refresh_settings(&app->session);
        if (result > 0 && action->edit_count > 0U) {
            snprintf(toast, sizeof toast, "Refreshing %d sections; %u edit%s kept, Y saves", result,
                     (unsigned)action->edit_count, action->edit_count == 1U ? "" : "s");
        } else if (result > 0) {
            snprintf(toast, sizeof toast, "Refreshing %d settings sections", result);
        } else if (result == 0) {
            snprintf(toast, sizeof toast, "%s", "Refresh already in progress");
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else {
            snprintf(toast, sizeof toast, "Refresh failed (%d)", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_SAVE_SETTINGS: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, "BLE transport unavailable");
            return;
        }
        mesh_app_save_settings(app, action, now);
        return;
    }
    case MESH_UI_ACTION_RADIO_ACTION: {
        /*
         * Reboot, shutdown and the three resets. Nothing here waits for an answer: the radio
         * acts a few seconds after acking and takes the link with it, so the toast says what
         * was asked for. A shutdown in particular has no reconnect to promise - the radio has
         * to be switched on by hand - so it says so rather than leaving auto-connect to look
         * broken while it retries a node that is off.
         */
        /* The two fixed-position rows are radio actions but not destructive ones: they are a
           save the user pressed for, they read the coordinate rows above them, and they are
           announced through the same "Saving ..." machinery a section save uses. */
        if ((enum mesh_ui_settings_action)action->number ==
                MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION ||
            (enum mesh_ui_settings_action)action->number ==
                MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION) {
            mesh_app_save_fixed_position(app, action, now);
            return;
        }
        enum mesh_admin_request_kind kind = MESH_ADMIN_REBOOT;
        const char *asked = "Rebooting; reconnecting shortly";
        switch ((enum mesh_ui_settings_action)action->number) {
        case MESH_UI_SETTINGS_ACTION_REBOOT:
            break;
        case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
            kind = MESH_ADMIN_SHUTDOWN;
            asked = "Shutting down; switch it on by hand";
            break;
        case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
            kind = MESH_ADMIN_RESET_NODEDB;
            asked = "Node database reset; favorites kept";
            break;
        case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
            kind = MESH_ADMIN_FACTORY_RESET_CONFIG;
            asked = "Factory reset sent; radio restarting";
            break;
        case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
            kind = MESH_ADMIN_FACTORY_RESET_DEVICE;
            asked = "Factory reset sent; forget it in Devices";
            break;
        default:
            return; /* a row the nav should never have confirmed */
        }
        const int result = mesh_session_radio_action(&app->session, kind);
        if (result > 0) {
            snprintf(toast, sizeof toast, "%s", asked);
        } else if (result == 0) {
            snprintf(toast, sizeof toast, "%s", "Already requested");
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else {
            snprintf(toast, sizeof toast, "Request failed (%d)", result);
            mesh_log_warn("ui", "Radio action %u failed: %d", (unsigned)action->number, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_TOGGLE_FAVORITE: {
        const bool favorite = (action->number != 0U);
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_set_node_favorite(&app->session, action->dest, favorite);
        if (result > 0) {
            snprintf(toast, sizeof toast, "%s %.20s", favorite ? "Pinned" : "Unpinned", name);
            mesh_log_info("ui", "%s node 0x%08x from the Nodes tab",
                          favorite ? "Pinned" : "Unpinned", action->dest);
        } else if (result == 0) {
            snprintf(toast, sizeof toast, "%.20s is already %s", name,
                     favorite ? "pinned" : "unpinned");
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", "That node is no longer in the list");
        } else {
            snprintf(toast, sizeof toast, "Pin failed (%d)", result);
            mesh_log_warn("ui", "Favorite for 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_REQUEST_NODE_INFO: {
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_request_node_info(&app->session, action->dest);
        if (result == 0) {
            /* Nothing here can promise an answer: the node may be out of range, asleep, or
               simply slow, and no ack comes back for the request itself. */
            snprintf(toast, sizeof toast, "Asked %.20s to introduce itself", name);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -EAGAIN) {
            /* Our own owner record has not landed yet, and sending a placeholder would erase
               this node's name on whoever received it. */
            snprintf(toast, sizeof toast, "%s", "Still syncing; try again in a moment");
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", "Cannot ask this node");
        } else {
            snprintf(toast, sizeof toast, "Request failed (%d)", result);
            mesh_log_warn("ui", "NodeInfo request for 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_TOGGLE_IGNORE: {
        const bool ignored = (action->number != 0U);
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_set_node_ignored(&app->session, action->dest, ignored);
        if (result > 0) {
            /* Said as what it does to the traffic, not as a preference that was recorded. */
            snprintf(toast, sizeof toast,
                     ignored ? "Dropping packets from %.20s" : "Hearing %.20s again", name);
            mesh_log_info("ui", "%s node 0x%08x from the Nodes tab",
                          ignored ? "Ignoring" : "Unignoring", action->dest);
        } else if (result == 0) {
            snprintf(toast, sizeof toast, "%.20s is already %s", name,
                     ignored ? "ignored" : "not ignored");
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", "That node is no longer in the list");
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", "Cannot ignore this node");
        } else {
            snprintf(toast, sizeof toast, "Ignore failed (%d)", result);
            mesh_log_warn("ui", "Ignore for 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_TOGGLE_MUTE: {
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_toggle_node_muted(&app->session, action->dest);
        if (result == 0) {
            /* Already on its way. Two local flips for one toggle on the wire would leave the
               row stating the opposite of what the radio is about to do. */
            snprintf(toast, sizeof toast, "%s", "Mute already requested");
        } else if (result > 0) {
            /* The session flipped the cached flag on the way through, so what it now holds is
               what we asked the radio for. */
            const struct mesh_ui_node_summary *node =
                mesh_ui_node_detail_find(&app->ui_store.handshake, action->dest);
            const bool muted = node != NULL ? node->is_muted : true;
            snprintf(toast, sizeof toast, "%s %.20s", muted ? "Muted" : "Unmuted", name);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", "That node is no longer in the list");
        } else {
            snprintf(toast, sizeof toast, "Mute failed (%d)", result);
            mesh_log_warn("ui", "Mute for 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_REMOVE_NODE: {
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_remove_node(&app->session, action->dest);
        if (result > 0) {
            /* Says how it comes back, because the row that would have undone it has gone with
               the node. */
            snprintf(toast, sizeof toast, "Removed %.14s; back when it speaks", name);
            mesh_log_info("ui", "Removed node 0x%08x from the Nodes tab", action->dest);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", "That node is no longer in the list");
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", "That is the radio you are connected to");
        } else {
            snprintf(toast, sizeof toast, "Remove failed (%d)", result);
            mesh_log_warn("ui", "Remove of 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_TRACEROUTE: {
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_send_traceroute(&app->session, action->dest);
        if (result == 0) {
            snprintf(toast, sizeof toast, "Tracing route to %.20s", name);
            mesh_log_info("ui", "Traceroute to 0x%08x from the Nodes tab", action->dest);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else if (result == -EBUSY) {
            /* One trace at a time is this client's half of the firmware's rate limit. */
            snprintf(toast, sizeof toast, "%s", "A traceroute is already running");
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", "Cannot trace a route to this node");
        } else {
            snprintf(toast, sizeof toast, "Traceroute failed (%d)", result);
            mesh_log_warn("ui", "Traceroute to 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_DISCONNECT: {
        struct mesh_transport *transport = mesh_app_active_transport();
        const char *identifier = mesh_app_connected_identifier();
        char name[64];
        snprintf(name, sizeof name, "%s",
                 action->identifier[0] != '\0' ? action->identifier
                                               : (identifier != NULL ? identifier : ""));

        int result = -ENOTCONN;
        if (transport == mesh_serial_transport()) {
            result = mesh_serial_transport_disconnect(transport);
        } else if (transport != NULL) {
            result = mesh_ble_transport_disconnect(transport);
        }

        if (result == 0) {
            /* Holding auto-connect is the point of the press: without it the next loop turn
               takes the same radio straight back. */
            app->autoconnect_held = true;
            app->ui_report_link_error = false;
            if (name[0] != '\0') {
                snprintf(toast, sizeof toast, "Disconnected from %.30s", name);
            } else {
                snprintf(toast, sizeof toast, "%s", "Disconnected");
            }
            mesh_log_info("ui", "Disconnect requested from the device (%s)",
                          name[0] != '\0' ? name : "active link");
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Nothing is connected");
        } else {
            snprintf(toast, sizeof toast, "Disconnect failed (%d)", result);
            mesh_log_warn("ui", "Disconnect failed: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_FORGET: {
        if (ble == NULL || action->kind != (uint8_t)MESH_UI_DEVICE_BLE) {
            mesh_ui_store_set_toast(&app->ui_store, now, "Only Bluetooth nodes are paired");
            return;
        }
        const bool was_connected =
            (mesh_app_connected_identifier() != NULL &&
             strcmp(mesh_app_connected_identifier(), action->identifier) == 0);
        const int result = mesh_ble_transport_forget(ble, action->identifier);
        if (result == 0) {
            /* The bond is gone, so a reconnect would only fail on StartNotify until the user
               pairs again; do not let auto-connect spend the next minute proving it. */
            if (was_connected) {
                app->autoconnect_held = true;
            }
            snprintf(toast, sizeof toast, "Forgot %.30s; pair again to use it", action->identifier);
            mesh_log_info("ui", "Forgot BLE node %s", action->identifier);
        } else if (mesh_transport_registry_take_error(&app->transport_registry, toast,
                                                      sizeof toast)) {
            mesh_log_warn("ui", "Forget %s failed: %s (%d)", action->identifier, toast, result);
        } else {
            snprintf(toast, sizeof toast, "Could not forget it (%d)", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_SUBMIT_PASSKEY: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, "BLE transport unavailable");
            return;
        }
        const unsigned long value = strtoul(action->text, NULL, 10);
        const int result = mesh_ble_transport_submit_passkey(ble, (uint32_t)value);
        if (result == 0) {
            snprintf(toast, sizeof toast, "%s", "Pairing...");
            /* Whatever goes wrong from here is reported by the transport, not by this call. */
            app->ui_report_link_error = true;
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", "The pairing request expired");
        } else {
            snprintf(toast, sizeof toast, "Pairing failed (%d)", result);
            mesh_log_warn("ui", "Passkey submit failed: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_CANCEL_PAIRING: {
        if (ble != NULL) {
            (void)mesh_ble_transport_cancel_pairing(ble);
        }
        /* A cancelled pairing is a cancelled connect: do not let auto-connect start it over. */
        app->autoconnect_held = true;
        app->ui_report_link_error = false;
        mesh_ui_store_set_toast(&app->ui_store, now, "Pairing cancelled");
        return;
    }
    case MESH_UI_ACTION_CYCLE_UPDATE_CHANNEL: {
        /* Steps DEFAULT -> STABLE -> PRERELEASE -> DEFAULT. Saved immediately rather than
           collected as a pending edit: About has no Y-save, because there is no radio write
           behind it. */
        const enum mesh_update_channel next = (enum mesh_update_channel)(
            ((unsigned)app->updater.channel + 1U) % (unsigned)MESH_UPDATE_CHANNEL_COUNT);
        if (!mesh_updater_set_channel(&app->updater, next)) {
            mesh_ui_store_set_toast(&app->ui_store, now, "Busy; try again in a moment");
            return;
        }
        app->ui_preferences.update_channel = (uint8_t)app->updater.channel;
        app->ui_preferences_dirty = true;
        snprintf(toast, sizeof toast, "Update channel: %.*s", (int)(sizeof toast - 18U),
                 mesh_update_channel_name(app->updater.channel));
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_TOGGLE_DEV_UPDATES: {
        if (!mesh_updater_set_allow_dev(&app->updater, !app->updater.allow_dev)) {
            mesh_ui_store_set_toast(&app->ui_store, now, "Busy; try again in a moment");
            return;
        }
        app->ui_preferences.update_allow_dev = app->updater.allow_dev;
        app->ui_preferences_dirty = true;
        mesh_ui_store_set_toast(&app->ui_store, now,
                                app->updater.allow_dev ? "Dev updates on; check again"
                                                       : "Dev updates off");
        return;
    }
    case MESH_UI_ACTION_CHECK_UPDATE: {
        const int result = mesh_updater_check(&app->updater, now);
        if (result == 0) {
            snprintf(toast, sizeof toast, "%s", "Checking for updates");
        } else if (result == -ENOTSUP) {
            mesh_str_copy(toast, sizeof toast,
                          app->updater.message[0] != '\0' ? app->updater.message
                                                          : "Updates are unavailable here");
        } else if (result == -EBUSY) {
            snprintf(toast, sizeof toast, "%s", "Already checking");
        } else {
            snprintf(toast, sizeof toast, "Update check failed (%d)", result);
            mesh_log_warn("ui", "Update check could not start: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_INSTALL_UPDATE: {
        const int result = mesh_updater_install(&app->updater, now);
        if (result == 0) {
            snprintf(toast, sizeof toast, "Downloading %.20s", app->updater.latest);
            mesh_log_info("ui", "Installing update %s from the About screen", app->updater.latest);
        } else if (result == -EBUSY) {
            snprintf(toast, sizeof toast, "%s", "Already working");
        } else if (result == -EINVAL) {
            /* Nothing to install: the check has not run, or found nothing newer. */
            snprintf(toast, sizeof toast, "%s", "Check for an update first");
        } else if (result == -ENOTSUP) {
            snprintf(toast, sizeof toast, "%s", "Updates are unavailable here");
        } else {
            snprintf(toast, sizeof toast, "Update failed (%d)", result);
            mesh_log_warn("ui", "Update install could not start: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_NONE:
    default:
        return;
    }
}
