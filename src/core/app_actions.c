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
#include "mesh/geo/coords.h"
#include "mesh/i18n/strings.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/serial.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/preferences.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
        mesh_app_note_connected_device(app, action->identifier, action->kind);
        /* Asking for a radio lifts a hold an earlier disconnect put on auto-connect. */
        app->autoconnect_held = false;
        app->autoconnect_failures = 0U;
        app->autoconnect_retry_at_ms = 0U;

        /* A user pick beats whatever auto-connect is doing or has done, on either link. */
        const int result = mesh_app_link_connect(app, action->identifier, action->kind);
        if (result == 0 || result == -EALREADY || result == -EINPROGRESS) {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_CONNECTING, action->identifier);
            /* BLE resolves services from tick(), so a 0 here is not yet a connection. Arm the
               error report so whatever goes wrong next reaches the screen. */
            app->ui_report_link_error = true;
        } else if (mesh_transport_registry_take_error(&app->transport_registry, toast,
                                                      sizeof toast)) {
            mesh_log_warn("ui", "Connect to %s failed: %s (%d)", action->identifier, toast, result);
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_CONNECT_FAILED, result);
            mesh_log_warn("ui", "Connect to %s failed: %d", action->identifier, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_SEND_TEXT: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, mesh_str(MESH_STR_TOAST_BLE_UNAVAILABLE));
            return;
        }
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
                ? mesh_session_send_reaction(&app->session, action->dest, action->channel,
                                             action->text, action->reply_id, &packet_id)
                : mesh_session_send_reply(&app->session, action->dest, action->channel,
                                          action->text, !broadcast, action->reply_id, &packet_id);
        if (result == 0 && action->is_reaction) {
            /* A tapback has no bubble and nothing to wait for, so it is not watched: there is
               no delivery mark for a report to land on. */
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_REACTION_SENT));
            mesh_log_info("ui", "Reacted \"%s\" to packet %u in %s", action->text, action->reply_id,
                          app->ui_store.nav.target_name);
        } else if (result == 0) {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_SENT_TO,
                            app->ui_store.nav.target_name);
            if (action->reply_id != 0U) {
                mesh_log_info("ui", "Sent \"%s\" to %s (packet %u, replying to %u)", action->text,
                              app->ui_store.nav.target_name, packet_id, action->reply_id);
            } else {
                mesh_log_info("ui", "Sent \"%s\" to %s (packet %u)", action->text,
                              app->ui_store.nav.target_name, packet_id);
            }
            mesh_app_watch_sent(app, packet_id, app->ui_store.nav.target_name);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NOT_CONNECTED));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_SEND_FAILED, result);
            mesh_log_warn("ui", "Send to %s failed: %d", app->ui_store.nav.target_name, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_REFRESH_SETTINGS: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, mesh_str(MESH_STR_TOAST_BLE_UNAVAILABLE));
            return;
        }
        const int result = mesh_session_refresh_settings(&app->session);
        if (result > 0 && action->edit_count > 0U) {
            mesh_str_format_plural(toast, sizeof toast, MESH_STR_TOAST_REFRESH_EDITS_ONE,
                                   action->edit_count, result, (unsigned)action->edit_count);
        } else if (result > 0) {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_REFRESHING, result);
        } else if (result == 0) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_REFRESH_IN_PROGRESS));
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NOT_CONNECTED));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_REFRESH_FAILED, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_SAVE_SETTINGS: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, mesh_str(MESH_STR_TOAST_BLE_UNAVAILABLE));
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
        /*
         * Asking a Store & Forward router for what we missed. Not an AdminMessage at all - it
         * is a packet to another node on the mesh - so it leaves before the admin queue below,
         * the way the fixed-position pair does.
         *
         * The toast says which of the two things the press did, because they take different
         * amounts of time: with a router already known it is one packet and an answer, and with
         * none it is a broadcast ping first and up to half a minute of waiting. Nothing here
         * announces the messages themselves; they arrive in the transcript, and the section's
         * own row counts them.
         */
        if ((enum mesh_ui_settings_action)action->number ==
            MESH_UI_SETTINGS_ACTION_REQUEST_HISTORY) {
            const struct mesh_store_forward *before = mesh_session_store_forward(&app->session);
            const bool knew_router = before != NULL && before->router != 0U;
            char name[MESH_UI_STORE_FORWARD_NAME_MAX];
            mesh_str_copy(name, sizeof name, app->ui_store.settings.store_forward.router_name);

            const int result = mesh_session_request_history(&app->session);
            char asked_toast[MESH_UI_NAV_TOAST_MAX];
            if (result == -EBUSY) {
                mesh_str_copy(asked_toast, sizeof asked_toast,
                              mesh_str(MESH_STR_TOAST_HISTORY_RUNNING));
            } else if (result < 0) {
                mesh_str_format(asked_toast, sizeof asked_toast, MESH_STR_TOAST_HISTORY_FAILED,
                                result);
            } else if (knew_router) {
                mesh_str_format(asked_toast, sizeof asked_toast, MESH_STR_TOAST_HISTORY_ASKED,
                                name);
            } else {
                mesh_str_copy(asked_toast, sizeof asked_toast,
                              mesh_str(MESH_STR_TOAST_HISTORY_LOOKING));
            }
            mesh_ui_store_set_toast(&app->ui_store, now, asked_toast);
            return;
        }
        enum mesh_admin_request_kind kind = MESH_ADMIN_REBOOT;
        enum mesh_str_id asked = MESH_STR_TOAST_REBOOTING;
        switch ((enum mesh_ui_settings_action)action->number) {
        case MESH_UI_SETTINGS_ACTION_REBOOT:
            break;
        case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
            kind = MESH_ADMIN_SHUTDOWN;
            asked = MESH_STR_TOAST_SHUTTING_DOWN;
            break;
        case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
            kind = MESH_ADMIN_RESET_NODEDB;
            /* Says what it did *not* touch as well: the Brick's own roster outliving the
               reset is the difference between the Status screen's 2 nodes and the Nodes
               tab's 81, and the row below the one just pressed is what clears it. */
            asked = MESH_STR_TOAST_NODEDB_RESET;
            break;
        case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
            kind = MESH_ADMIN_FACTORY_RESET_CONFIG;
            asked = MESH_STR_TOAST_FACTORY_CONFIG;
            break;
        case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
            kind = MESH_ADMIN_FACTORY_RESET_DEVICE;
            asked = MESH_STR_TOAST_FACTORY_DEVICE;
            break;
        /* The backup trio. Like the resets, nothing comes back to confirm any of them: the
           firmware answers the request and then acts, so the toast says what was asked for.
           A restore is the one that changes what the radio holds, and the settings it changes
           are the ones this tab is showing - so it is followed by a refresh rather than left
           to a screen that would keep drawing the values it had before. */
        case MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG:
            kind = MESH_ADMIN_BACKUP_PREFERENCES;
            asked = MESH_STR_TOAST_BACKED_UP;
            break;
        case MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG:
            kind = MESH_ADMIN_RESTORE_PREFERENCES;
            asked = MESH_STR_TOAST_RESTORED;
            break;
        case MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP:
            kind = MESH_ADMIN_REMOVE_BACKUP_PREFERENCES;
            asked = MESH_STR_TOAST_BACKUP_REMOVED;
            break;
        default:
            return; /* a row the nav should never have confirmed */
        }
        const int result = mesh_session_radio_action(&app->session, kind);
        if (result > 0) {
            snprintf(toast, sizeof toast, "%s", mesh_str(asked));
            if (kind == MESH_ADMIN_RESTORE_PREFERENCES) {
                (void)mesh_session_refresh_settings(&app->session);
            }
        } else if (result == 0) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_ALREADY_REQUESTED));
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NOT_CONNECTED));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_REQUEST_FAILED, result);
            mesh_log_warn("ui", "Radio action %u failed: %d", (unsigned)action->number, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_FORGET_NODES: {
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
            mesh_str_format_plural(toast, sizeof toast, MESH_STR_TOAST_FORGOT_NODES_ONE,
                                   (uint32_t)dropped, dropped);
            mesh_log_info("ui", "Forgot %d cached node%s from Settings (%s)", dropped,
                          dropped == 1 ? "" : "s", all ? "all" : "off-radio only");
        } else if (dropped == 0) {
            snprintf(toast, sizeof toast, "%s",
                     mesh_str(all ? MESH_STR_TOAST_NOTHING_CACHED : MESH_STR_TOAST_ALL_ON_RADIO));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_FORGET_FAILED, dropped);
            mesh_log_warn("ui", "Forget nodes failed: %d", dropped);
        }
        /* The hint that sent the user here compares each sync against the last, so the press
           that acts on it has to move that baseline too: without this, a divergence the user
           has just cleared would still be the number the next sync is measured against. */
        app->ui_nodes_off_radio_seen = mesh_session_forgettable_nodes(&app->session, true);
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        mesh_app_publish_ui_state(app);
        return;
    }
    case MESH_UI_ACTION_TOGGLE_FAVORITE: {
        const bool favorite = (action->number != 0U);
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = mesh_session_set_node_favorite(&app->session, action->dest, favorite);
        if (result > 0) {
            mesh_str_format(toast, sizeof toast,
                            favorite ? MESH_STR_TOAST_PINNED : MESH_STR_TOAST_UNPINNED, name);
            mesh_log_info("ui", "%s node 0x%08x from the Nodes tab",
                          favorite ? "Pinned" : "Unpinned", action->dest);
        } else if (result == 0) {
            mesh_str_format(
                toast, sizeof toast,
                favorite ? MESH_STR_TOAST_ALREADY_PINNED : MESH_STR_TOAST_ALREADY_UNPINNED, name);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NOT_CONNECTED));
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NODE_GONE));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_PIN_FAILED, result);
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
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_ASKED_NAME, name);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NOT_CONNECTED));
        } else if (result == -EAGAIN) {
            /* Our own owner record has not landed yet, and sending a placeholder would erase
               this node's name on whoever received it. */
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_STILL_SYNCING));
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_CANNOT_ASK));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_REQUEST_FAILED, result);
            mesh_log_warn("ui", "NodeInfo request for 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_REQUEST_POSITION:
    case MESH_UI_ACTION_REQUEST_TELEMETRY: {
        const bool position = (action->type == MESH_UI_ACTION_REQUEST_POSITION);
        char name[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), action->dest, name,
                                  sizeof name);
        const int result = position ? mesh_session_request_position(&app->session, action->dest)
                                    : mesh_session_request_telemetry(&app->session, action->dest);
        if (result == 0) {
            /* Nothing here can promise an answer either: the request carries no want_ack, and a
               node that is out of range, asleep or simply not equipped answers nothing. */
            mesh_str_format(
                toast, sizeof toast,
                position ? MESH_STR_TOAST_ASKED_POSITION : MESH_STR_TOAST_ASKED_TELEMETRY, name);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NOT_CONNECTED));
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_CANNOT_ASK));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_REQUEST_FAILED, result);
            mesh_log_warn("ui", "%s request for 0x%08x failed: %d",
                          position ? "Position" : "Telemetry", action->dest, result);
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
            mesh_str_format(toast, sizeof toast,
                            ignored ? MESH_STR_TOAST_IGNORING : MESH_STR_TOAST_UNIGNORING, name);
            mesh_log_info("ui", "%s node 0x%08x from the Nodes tab",
                          ignored ? "Ignoring" : "Unignoring", action->dest);
        } else if (result == 0) {
            mesh_str_format(
                toast, sizeof toast,
                ignored ? MESH_STR_TOAST_ALREADY_IGNORED : MESH_STR_TOAST_ALREADY_UNIGNORED, name);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NOT_CONNECTED));
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NODE_GONE));
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_CANNOT_IGNORE));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_IGNORE_FAILED, result);
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
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_MUTE_REQUESTED));
        } else if (result > 0) {
            /* The session flipped the cached flag on the way through, so what it now holds is
               what we asked the radio for. */
            const struct mesh_ui_node_summary *node =
                mesh_ui_node_detail_find(&app->ui_store.handshake, action->dest);
            const bool muted = node != NULL ? node->is_muted : true;
            mesh_str_format(toast, sizeof toast,
                            muted ? MESH_STR_TOAST_MUTED : MESH_STR_TOAST_UNMUTED, name);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NOT_CONNECTED));
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NODE_GONE));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_MUTE_FAILED, result);
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
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_REMOVED_NODE, name);
            mesh_log_info("ui", "Removed node 0x%08x from the Nodes tab", action->dest);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NOT_CONNECTED));
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NODE_GONE));
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_REMOVE_SELF));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_REMOVE_FAILED, result);
            mesh_log_warn("ui", "Remove of 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_DELETE_CONVERSATION: {
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
        const uint32_t removed = mesh_ui_store_forget_conversation(
            &app->ui_store, (uint8_t)action->number, action->dest, action->channel);

        /* Written straight back out, so the messages do not come back on the next start. */
        if (app->ui_handshake_cache_path[0] != '\0') {
            app->ui_handshake_cache_dirty = true;
            mesh_app_flush_ui_cache(app);
        }

        if (removed > 0U) {
            mesh_str_format_plural(toast, sizeof toast, MESH_STR_TOAST_DELETED_MESSAGES_ONE,
                                   removed, (unsigned)removed, name);
            mesh_log_info("ui", "Deleted %u message(s) in the conversation with %s",
                          (unsigned)removed, name);
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_NOTHING_TO_DELETE, name);
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
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_TRACING, name);
            mesh_log_info("ui", "Traceroute to 0x%08x from the Nodes tab", action->dest);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NOT_CONNECTED));
        } else if (result == -EBUSY) {
            /* One trace at a time is this client's half of the firmware's rate limit. */
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_TRACE_RUNNING));
        } else if (result == -EINVAL) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_CANNOT_TRACE));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_TRACE_FAILED, result);
            mesh_log_warn("ui", "Traceroute to 0x%08x failed: %d", action->dest, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_SHARE_WAYPOINT: {
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
                snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_WAYPOINT_GONE));
                mesh_ui_store_set_toast(&app->ui_store, now, toast);
                return;
            }
            waypoint = *existing;
            channel = existing->channel;
        } else {
            if (action->text[0] == '\0') {
                snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_WAYPOINT_NAME_NEEDED));
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
                snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_WAYPOINT_NO_FIX));
                mesh_ui_store_set_toast(&app->ui_store, now, toast);
                return;
            }
            waypoint.has_coords = true;
            waypoint.latitude_i = node->position.latitude_i;
            waypoint.longitude_i = node->position.longitude_i;
            mesh_str_copy(waypoint.name, sizeof waypoint.name, action->text);
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
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_WAYPOINT_SHARED, waypoint.name);
            mesh_log_info("ui", "Shared waypoint %u on channel %u", id, (unsigned)channel);
        } else if (result == -ENOTCONN) {
            /* The session kept the place regardless - see mesh_session_send_waypoint(). What
               did not happen is the mesh hearing about it, and that is what the toast says. */
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_WAYPOINT_SAVED, waypoint.name);
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_WAYPOINT_FAILED, result);
            mesh_log_warn("ui", "Sharing a waypoint failed: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        mesh_app_publish_ui_state(app);
        return;
    }
    case MESH_UI_ACTION_FORGET_WAYPOINT: {
        const struct mesh_waypoint *existing =
            mesh_waypoint_book_get(mesh_session_waypoints(&app->session), action->number);
        char name[MESH_WAYPOINT_NAME_MAX + 1U];
        mesh_str_copy(name, sizeof name,
                      (existing != NULL && existing->name[0] != '\0')
                          ? existing->name
                          : mesh_str(MESH_STR_WAYPOINTS_UNNAMED));

        bool shared = false;
        const int result = mesh_session_forget_waypoint(&app->session, action->number, &shared);
        if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_WAYPOINT_GONE));
        } else if (shared) {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_WAYPOINT_DELETED, name);
        } else {
            /* The place is gone from here either way; the toast says whether the mesh heard
               about it, because a locked place and a dead link both end up here. */
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_WAYPOINT_FORGOT, name);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        mesh_app_publish_ui_state(app);
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
                mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_DISCONNECTED_FROM, name);
            } else {
                snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_DISCONNECTED));
            }
            mesh_log_info("ui", "Disconnect requested from the device (%s)",
                          name[0] != '\0' ? name : "active link");
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_NOTHING_CONNECTED));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_DISCONNECT_FAILED, result);
            mesh_log_warn("ui", "Disconnect failed: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_FORGET: {
        if (ble == NULL || action->kind != (uint8_t)MESH_UI_DEVICE_BLE) {
            mesh_ui_store_set_toast(&app->ui_store, now, mesh_str(MESH_STR_TOAST_BLE_ONLY_PAIRING));
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
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_FORGOT_DEVICE, action->identifier);
            mesh_log_info("ui", "Forgot BLE node %s", action->identifier);
        } else if (mesh_transport_registry_take_error(&app->transport_registry, toast,
                                                      sizeof toast)) {
            mesh_log_warn("ui", "Forget %s failed: %s (%d)", action->identifier, toast, result);
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_FORGET_DEVICE_FAILED, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_SUBMIT_PASSKEY: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, mesh_str(MESH_STR_TOAST_BLE_UNAVAILABLE));
            return;
        }
        const unsigned long value = strtoul(action->text, NULL, 10);
        const int result = mesh_ble_transport_submit_passkey(ble, (uint32_t)value);
        if (result == 0) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_PAIRING));
            /* Whatever goes wrong from here is reported by the transport, not by this call. */
            app->ui_report_link_error = true;
        } else if (result == -ENOENT) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_PAIRING_EXPIRED));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_PAIRING_FAILED, result);
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
        mesh_ui_store_set_toast(&app->ui_store, now, mesh_str(MESH_STR_TOAST_PAIRING_CANCELLED));
        return;
    }
    case MESH_UI_ACTION_CYCLE_UPDATE_CHANNEL: {
        /* Steps DEFAULT -> STABLE -> PRERELEASE -> DEFAULT. Saved immediately rather than
           collected as a pending edit: About has no Y-save, because there is no radio write
           behind it. */
        const enum mesh_update_channel next = (enum mesh_update_channel)(
            ((unsigned)app->updater.channel + 1U) % (unsigned)MESH_UPDATE_CHANNEL_COUNT);
        if (!mesh_updater_set_channel(&app->updater, next)) {
            mesh_ui_store_set_toast(&app->ui_store, now, mesh_str(MESH_STR_TOAST_BUSY_RETRY));
            return;
        }
        app->ui_preferences.update_channel = (uint8_t)app->updater.channel;
        app->ui_preferences_dirty = true;
        mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_UPDATE_CHANNEL,
                        (int)(sizeof toast - 18U), mesh_update_channel_name(app->updater.channel));
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_CYCLE_LANGUAGE: {
        if (mesh_i18n_is_overridden()) {
            return;
        }
        const size_t count = mesh_i18n_locale_count();
        for (size_t i = 0; i < count; ++i) {
            if (mesh_i18n_locale_at(i) != mesh_i18n_locale()) {
                continue;
            }
            const struct mesh_i18n_locale *next = mesh_i18n_locale_at((i + 1U) % count);
            (void)mesh_i18n_set_locale(next->id);
            mesh_str_copy(app->ui_preferences.language, sizeof app->ui_preferences.language,
                          next->id);
            app->ui_preferences_dirty = true;
            mesh_ui_store_set_toast(&app->ui_store, now, next->name);
            /* Persist and rebuild formatted snapshot text before the queued redraw. */
            mesh_app_publish_ui_state(app);
            break;
        }
        return;
    }
    case MESH_UI_ACTION_CYCLE_THEME: {
        /* Saved immediately rather than collected as a pending edit, for the reason the update
           channel is: About has no Y-save, because there is no radio write behind it.
           The frame after this one is drawn in the new theme - the backends read it out of the
           client info in the snapshot - so the press is its own confirmation. */
        if (app->ui_theme_from_env) {
            mesh_ui_store_set_toast(&app->ui_store, now, mesh_str(MESH_STR_TOAST_THEME_HELD));
            return;
        }
        const struct mesh_ui_theme *next = mesh_ui_theme_next(app->ui_theme);
        if (next == NULL) {
            return;
        }
        app->ui_theme = next;
        mesh_str_copy(app->ui_preferences.theme, sizeof app->ui_preferences.theme, next->id);
        app->ui_preferences_dirty = true;
        mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_THEME, (int)(sizeof toast - 8U),
                        next->name);
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        /*
         * Published here rather than left to the next loop turn. This handler runs inside
         * mesh_event_loop_run(), and the toast above has already queued a redraw that the same
         * turn will drain - so without this the press's own frame would arrive with the new
         * toast drawn in the old theme, and the switch would land a turn later. That gap is
         * exactly what "the frame the press draws is the answer" is not.
         */
        mesh_app_publish_ui_state(app);
        return;
    }
    case MESH_UI_ACTION_TOGGLE_DEV_UPDATES: {
        if (!mesh_updater_set_allow_dev(&app->updater, !app->updater.allow_dev)) {
            mesh_ui_store_set_toast(&app->ui_store, now, mesh_str(MESH_STR_TOAST_BUSY_RETRY));
            return;
        }
        app->ui_preferences.update_allow_dev = app->updater.allow_dev;
        app->ui_preferences_dirty = true;
        mesh_ui_store_set_toast(&app->ui_store, now,
                                mesh_str(app->updater.allow_dev ? MESH_STR_TOAST_DEV_UPDATES_ON
                                                                : MESH_STR_TOAST_DEV_UPDATES_OFF));
        return;
    }
    case MESH_UI_ACTION_CHECK_UPDATE: {
        const int result = mesh_updater_check(&app->updater, now);
        if (result == 0) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_CHECKING_UPDATES));
        } else if (result == -ENOTSUP) {
            mesh_str_copy(toast, sizeof toast,
                          app->updater.message[0] != '\0'
                              ? app->updater.message
                              : mesh_str(MESH_STR_TOAST_UPDATES_UNAVAILABLE));
        } else if (result == -EBUSY) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_ALREADY_CHECKING));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_UPDATE_CHECK_FAILED, result);
            mesh_log_warn("ui", "Update check could not start: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_CYCLE_FIRMWARE_CHANNEL: {
        const enum mesh_firmware_channel next = (enum mesh_firmware_channel)(
            ((unsigned)app->firmware.channel + 1U) % (unsigned)MESH_FIRMWARE_CHANNEL_COUNT);
        if (!mesh_firmware_set_channel(&app->firmware, next)) {
            /* Refused, which here only ever means a check is in flight. */
            mesh_ui_store_set_toast(&app->ui_store, now, mesh_str(MESH_STR_TOAST_ALREADY_CHECKING));
            return;
        }
        app->ui_preferences.firmware_channel = (uint8_t)app->firmware.channel;
        app->ui_preferences_dirty = true;
        mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_FIRMWARE_CHANNEL,
                        mesh_firmware_channel_name(app->firmware.channel));
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_CHECK_RADIO_FIRMWARE: {
        /* What the radio said about itself is the whole input: the model number decides which
           board this is and the version decides whether the newest release is news. Both may
           be absent, and a check on either still reports what upstream has published - see
           mesh_firmware_check(). */
        const struct mesh_radio_settings *const settings = mesh_session_settings(&app->session);
        const bool known = settings != NULL && settings->has_metadata;
        const int result =
            mesh_firmware_check(&app->firmware, known ? (uint32_t)settings->metadata.hw_model : 0U,
                                known ? settings->metadata.firmware_version : "", now);
        if (result == 0) {
            mesh_str_copy(toast, sizeof toast, mesh_str(MESH_STR_TOAST_CHECKING_FIRMWARE));
        } else if (result == -ENOTSUP) {
            mesh_str_copy(toast, sizeof toast, mesh_str(MESH_STR_TOAST_UPDATES_UNAVAILABLE));
        } else if (result == -EBUSY) {
            mesh_str_copy(toast, sizeof toast, mesh_str(MESH_STR_TOAST_ALREADY_CHECKING));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_UPDATE_CHECK_FAILED, result);
            mesh_log_warn("ui", "Firmware check could not start: %d", result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_INSTALL_UPDATE: {
        const int result = mesh_updater_install(&app->updater, now);
        if (result == 0) {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_DOWNLOADING, app->updater.latest);
            mesh_log_info("ui", "Installing update %s from the About screen", app->updater.latest);
        } else if (result == -EBUSY) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_ALREADY_WORKING));
        } else if (result == -EINVAL) {
            /* Nothing to install: the check has not run, or found nothing newer. */
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_CHECK_FIRST));
        } else if (result == -ENOTSUP) {
            snprintf(toast, sizeof toast, "%s", mesh_str(MESH_STR_TOAST_UPDATES_UNAVAILABLE));
        } else {
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_UPDATE_FAILED, result);
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
