#include "mesh/transport/ble_bluez.h"

#include "mesh/core/event_loop.h"
#include "mesh/utils/array.h"
#include "mesh/utils/log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>

#define MESH_BLUEZ_READ_TIMEOUT_MS 3000
#define MESH_BLUEZ_PROPERTY_TIMEOUT_MS 1000
/* Our org.bluez.Agent1 object. BlueZ calls back on this path to ask for a PIN. */
#define MESH_BLUEZ_AGENT_PATH "/org/meshclient/agent"

#ifdef MESH_HAVE_DBUS
#include <dbus/dbus.h>

static uint32_t mesh_bluez_watch_flags_to_events(unsigned int flags) {
    uint32_t events = 0U;
    if ((flags & DBUS_WATCH_READABLE) != 0U) {
        events |= EPOLLIN;
    }
    if ((flags & DBUS_WATCH_WRITABLE) != 0U) {
        events |= EPOLLOUT;
    }
    if ((flags & DBUS_WATCH_ERROR) != 0U) {
        events |= EPOLLERR;
    }
    if ((flags & DBUS_WATCH_HANGUP) != 0U) {
        events |= EPOLLHUP;
    }
    return events;
}

/*
 * Turns a BlueZ D-Bus error into an errno the transport can act on. Everything used to come
 * back as -EIO, which left the UI unable to tell "this node needs pairing" - the one failure
 * the user can actually fix - apart from a radio that is simply out of range. BlueZ is not
 * consistent about which name it uses (StartNotify on an unpaired node answers
 * org.bluez.Error.Failed with "Not paired"), so the message is checked too.
 */
static int mesh_bluez_error_to_errno(const char *name, const char *message) {
    if (name != NULL) {
        const char *suffix = strrchr(name, '.');
        suffix = (suffix != NULL) ? suffix + 1 : name;
        if (strcmp(suffix, "NotPaired") == 0 || strcmp(suffix, "NotPermitted") == 0 ||
            strcmp(suffix, "NotAuthorized") == 0 || strncmp(suffix, "Authentication", 14) == 0) {
            return -EACCES;
        }
        if (strcmp(suffix, "NotConnected") == 0 || strcmp(suffix, "NotReady") == 0) {
            return -ENOTCONN;
        }
        if (strcmp(suffix, "InProgress") == 0) {
            return -EBUSY;
        }
        if (strcmp(suffix, "NoReply") == 0 || strcmp(suffix, "Timeout") == 0 ||
            strcmp(suffix, "TimedOut") == 0) {
            return -ETIMEDOUT;
        }
        if (strcmp(suffix, "DoesNotExist") == 0 || strcmp(suffix, "UnknownObject") == 0) {
            return -ENOENT;
        }
    }
    if (message != NULL) {
        if (strcasecmp(message, "Not paired") == 0 || strcasecmp(message, "Not Authorized") == 0) {
            return -EACCES;
        }
        if (strcasecmp(message, "Page Timeout") == 0 ||
            strcasecmp(message, "Connection Timeout") == 0) {
            return -ETIMEDOUT;
        }
    }
    return -EIO;
}

static int mesh_bluez_dbus_error_to_errno(const DBusError *error) {
    if (error == NULL || !dbus_error_is_set(error)) {
        return -EIO;
    }
    return mesh_bluez_error_to_errno(error->name, error->message);
}

static int mesh_bluez_watch_fd_callback(int fd, uint32_t events, void *userdata);

static int mesh_bluez_watch_sync(struct mesh_bluez_client *client, size_t index) {
    if (client == NULL || index >= MESH_ARRAY_LEN(client->watches)) {
        return -EINVAL;
    }

    struct mesh_event_loop *loop = client->loop;
    if (loop == NULL) {
        return 0;
    }

    struct mesh_bluez_watch_entry *entry = &client->watches[index];
    if (entry->watch == NULL) {
        return 0;
    }

    entry->fd = dbus_watch_get_unix_fd(entry->watch);
    entry->events = mesh_bluez_watch_flags_to_events(dbus_watch_get_flags(entry->watch));
    entry->client = client;

    if (!dbus_watch_get_enabled(entry->watch)) {
        if (entry->registered) {
            mesh_event_loop_remove_fd(loop, entry->fd);
            entry->registered = false;
        }
        return 0;
    }

    if (entry->registered) {
        mesh_event_loop_update_fd(loop, entry->fd, entry->events);
        return 0;
    }

    int result =
        mesh_event_loop_add_fd(loop, entry->fd, entry->events, mesh_bluez_watch_fd_callback, entry);
    if (result < 0) {
        mesh_log_warn("bluez", "Failed to register D-Bus watch fd %d: %d", entry->fd, result);
        return result;
    }

    entry->registered = true;
    return 0;
}

static void mesh_bluez_watch_unregister(struct mesh_bluez_client *client, size_t index) {
    if (client == NULL || client->loop == NULL || index >= MESH_ARRAY_LEN(client->watches)) {
        return;
    }

    struct mesh_bluez_watch_entry *entry = &client->watches[index];
    if (!entry->registered) {
        return;
    }

    mesh_event_loop_remove_fd(client->loop, entry->fd);
    entry->registered = false;
}

static ssize_t mesh_bluez_watch_find(struct mesh_bluez_client *client, DBusWatch *watch) {
    if (client == NULL) {
        return -1;
    }

    for (size_t i = 0; i < MESH_ARRAY_LEN(client->watches); ++i) {
        if (client->watches[i].watch == watch) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static dbus_bool_t mesh_bluez_watch_add(DBusWatch *watch, void *userdata) {
    struct mesh_bluez_client *client = (struct mesh_bluez_client *)userdata;
    if (client == NULL || watch == NULL) {
        return FALSE;
    }

    for (size_t i = 0; i < MESH_ARRAY_LEN(client->watches); ++i) {
        if (client->watches[i].watch == NULL) {
            client->watches[i].watch = watch;
            client->watches[i].registered = false;
            client->watches[i].client = client;
            mesh_bluez_watch_sync(client, i);
            return TRUE;
        }
    }

    mesh_log_warn("bluez", "No space for additional D-Bus watches");
    return FALSE;
}

static void mesh_bluez_watch_remove(DBusWatch *watch, void *userdata) {
    struct mesh_bluez_client *client = (struct mesh_bluez_client *)userdata;
    if (client == NULL || watch == NULL) {
        return;
    }

    ssize_t index = mesh_bluez_watch_find(client, watch);
    if (index < 0) {
        return;
    }

    mesh_bluez_watch_unregister(client, (size_t)index);
    struct mesh_bluez_watch_entry *entry = &client->watches[index];
    entry->watch = NULL;
    entry->fd = -1;
    entry->events = 0U;
    entry->registered = false;
    entry->client = NULL;
}

static void mesh_bluez_watch_toggled(DBusWatch *watch, void *userdata) {
    struct mesh_bluez_client *client = (struct mesh_bluez_client *)userdata;
    if (client == NULL || watch == NULL) {
        return;
    }

    ssize_t index = mesh_bluez_watch_find(client, watch);
    if (index < 0) {
        return;
    }

    mesh_bluez_watch_sync(client, (size_t)index);
}

static int mesh_bluez_watch_fd_callback(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct mesh_bluez_watch_entry *entry = (struct mesh_bluez_watch_entry *)userdata;
    if (entry == NULL || entry->client == NULL || entry->watch == NULL) {
        return 0;
    }

    unsigned int flags = 0U;
    if ((events & EPOLLIN) != 0U) {
        flags |= DBUS_WATCH_READABLE;
    }
    if ((events & EPOLLOUT) != 0U) {
        flags |= DBUS_WATCH_WRITABLE;
    }
    if ((events & EPOLLERR) != 0U) {
        flags |= DBUS_WATCH_ERROR;
    }
    if ((events & EPOLLHUP) != 0U) {
        flags |= DBUS_WATCH_HANGUP;
    }

    if (!dbus_watch_handle(entry->watch, flags)) {
        mesh_log_warn("bluez", "dbus_watch_handle returned false");
    }

    mesh_bluez_client_process(entry->client);
    return 0;
}
#endif

struct mesh_bluez_mock_state {
    bool enabled;
    struct mesh_bluez_mock_config config;
    struct mesh_bluez_client *client;
    size_t read_cursor;
    unsigned services_resolved_polls;
    unsigned connect_polls;
    unsigned connected_polls;
    unsigned write_calls;
    unsigned pair_polls;
    /* Addresses the mock has bonded, so a device reads back Paired the way BlueZ would once
       the pairing completed. */
    char paired_addresses[4][32];
    size_t paired_count;
};

static struct mesh_bluez_mock_state g_mock_state;

/* "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0C" -> "AA:BB:CC:DD:EE:0C" */
static void mesh_bluez_mock_note_paired(const char *device_path) {
    const char *dev = (device_path != NULL) ? strstr(device_path, "/dev_") : NULL;
    if (dev == NULL || g_mock_state.paired_count >= 4U) {
        return;
    }
    dev += 5;
    char *out = g_mock_state.paired_addresses[g_mock_state.paired_count];
    size_t written = 0U;
    while (*dev != '\0' && *dev != '/' && written + 1U < 32U) {
        out[written++] = (*dev == '_') ? ':' : *dev;
        dev++;
    }
    out[written] = '\0';
    g_mock_state.paired_count++;
}

static bool mesh_bluez_mock_is_paired(const char *address) {
    for (size_t i = 0; i < g_mock_state.paired_count; ++i) {
        if (strcmp(g_mock_state.paired_addresses[i], address) == 0) {
            return true;
        }
    }
    return false;
}

static void mesh_bluez_apply_mock_devices(struct mesh_bluez_device_info *devices, size_t capacity,
                                          size_t *count) {
    if (devices == NULL || count == NULL) {
        return;
    }
    *count = 0;
    if (!g_mock_state.enabled || g_mock_state.config.devices == NULL) {
        return;
    }
    size_t to_copy = g_mock_state.config.device_count;
    if (to_copy > capacity) {
        to_copy = capacity;
    }
    for (size_t i = 0; i < to_copy; ++i) {
        devices[i] = g_mock_state.config.devices[i];
        if (!devices[i].paired && mesh_bluez_mock_is_paired(devices[i].address)) {
            devices[i].paired = true;
        }
    }
    *count = to_copy;
}

void mesh_bluez_client_mock_enable(const struct mesh_bluez_mock_config *config) {
    g_mock_state.enabled = true;
    if (config != NULL) {
        g_mock_state.config = *config;
    } else {
        memset(&g_mock_state.config, 0, sizeof(g_mock_state.config));
    }
    g_mock_state.client = NULL;
    g_mock_state.read_cursor = 0U;
    g_mock_state.services_resolved_polls = 0U;
    g_mock_state.connect_polls = 0U;
    g_mock_state.connected_polls = 0U;
    g_mock_state.write_calls = 0U;
    g_mock_state.pair_polls = 0U;
    memset(g_mock_state.paired_addresses, 0, sizeof(g_mock_state.paired_addresses));
    g_mock_state.paired_count = 0U;
}

void mesh_bluez_client_mock_disable(void) {
    g_mock_state.enabled = false;
    memset(&g_mock_state.config, 0, sizeof(g_mock_state.config));
    g_mock_state.client = NULL;
    g_mock_state.read_cursor = 0U;
    g_mock_state.services_resolved_polls = 0U;
    g_mock_state.connect_polls = 0U;
    g_mock_state.connected_polls = 0U;
    g_mock_state.write_calls = 0U;
    g_mock_state.pair_polls = 0U;
    memset(g_mock_state.paired_addresses, 0, sizeof(g_mock_state.paired_addresses));
    g_mock_state.paired_count = 0U;
}

int mesh_bluez_client_init(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return -EINVAL;
    }

    client->connection = NULL;
    client->connected = false;
    client->loop = NULL;
    client->notification_callback = NULL;
    client->notification_userdata = NULL;
    client->notify_characteristic_path[0] = '\0';
    client->pair_state = 0;
    client->pair_serial = 0U;
    client->pair_result = 0;
    client->pair_device_path[0] = '\0';
    client->agent_registered = false;
    client->agent_pending_message = NULL;
    memset(&client->agent_request, 0, sizeof(client->agent_request));
#ifdef MESH_HAVE_DBUS
    memset(client->watches, 0, sizeof(client->watches));
#endif

#ifdef MESH_HAVE_DBUS
    if (g_mock_state.enabled) {
        if (g_mock_state.config.init_result < 0) {
            return g_mock_state.config.init_result;
        }
        client->connected = true;
        g_mock_state.client = client;
        return 0;
    }

    DBusError error;
    dbus_error_init(&error);

    DBusConnection *connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (connection == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "Failed to connect to system bus: %s", error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }

    dbus_connection_set_exit_on_disconnect(connection, false);

    if (!dbus_connection_set_watch_functions(connection, mesh_bluez_watch_add,
                                             mesh_bluez_watch_remove, mesh_bluez_watch_toggled,
                                             client, NULL)) {
        dbus_connection_unref(connection);
        return -EIO;
    }

    client->connection = connection;
    client->connected = true;
    g_mock_state.client = NULL;
    return 0;
#else
    if (g_mock_state.enabled) {
        if (g_mock_state.config.init_result < 0) {
            return g_mock_state.config.init_result;
        }
        client->connected = true;
        g_mock_state.client = client;
        return 0;
    }
    mesh_log_debug("bluez", "DBus support disabled at build time");
    return -ENOSYS;
#endif
}

void mesh_bluez_client_mock_emit_notification(const char *char_path, const uint8_t *data,
                                              size_t len) {
    if (!g_mock_state.enabled) {
        return;
    }

    struct mesh_bluez_client *client = g_mock_state.client;
    if (client == NULL || data == NULL || len == 0U) {
        return;
    }

    if (char_path != NULL && client->notify_characteristic_path[0] != '\0' &&
        strcmp(char_path, client->notify_characteristic_path) != 0) {
        return;
    }

    if (client->notification_callback != NULL) {
        client->notification_callback(data, len, client->notification_userdata);
    }
}

void mesh_bluez_client_shutdown(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return;
    }

    /* Give BlueZ its agent back before the connection goes: a registration left behind on a
       name that has vanished blocks the next one with AlreadyExists. */
    mesh_bluez_client_unregister_agent(client);
    client->pair_state = 0;
    client->pair_serial = 0U;
    client->pair_device_path[0] = '\0';

#ifdef MESH_HAVE_DBUS
    if (client->loop != NULL) {
        mesh_bluez_client_detach_loop(client);
    }
#endif

#ifdef MESH_HAVE_DBUS
    if (client->connected && client->connection != NULL) {
        DBusConnection *connection = (DBusConnection *)client->connection;
        dbus_connection_set_watch_functions(connection, NULL, NULL, NULL, NULL, NULL);
        dbus_connection_unref(connection);
    }
#endif

    client->connection = NULL;
    client->connected = false;
    client->loop = NULL;
    client->notification_callback = NULL;
    client->notification_userdata = NULL;
    client->notify_characteristic_path[0] = '\0';
#ifdef MESH_HAVE_DBUS
    memset(client->watches, 0, sizeof(client->watches));
#endif
    if (g_mock_state.client == client) {
        g_mock_state.client = NULL;
    }
}

int mesh_bluez_client_check_ready(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return -EINVAL;
    }

#ifdef MESH_HAVE_DBUS
    if (g_mock_state.enabled) {
        return g_mock_state.config.check_ready_result;
    }

    if (!client->connected || client->connection == NULL) {
        return -ENOTCONN;
    }

    DBusConnection *connection = (DBusConnection *)client->connection;

    DBusError error;
    dbus_error_init(&error);
    dbus_bool_t has_owner = dbus_bus_name_has_owner(connection, "org.bluez", &error);

    if (dbus_error_is_set(&error)) {
        mesh_log_warn("bluez", "Failed to query BlueZ ownership: %s", error.message);
        dbus_error_free(&error);
        return -EIO;
    }

    if (!has_owner) {
        return -ENODEV;
    }

    return 0;
#else
    if (g_mock_state.enabled) {
        return g_mock_state.config.check_ready_result;
    }
    (void)client;
    return -ENOSYS;
#endif
}

#ifdef MESH_HAVE_DBUS
static void mesh_bluez_client_handle_properties_changed(struct mesh_bluez_client *client,
                                                        DBusMessage *message) {
    if (client == NULL || message == NULL) {
        return;
    }

    if (client->notify_characteristic_path[0] == '\0') {
        return;
    }

    const char *path = dbus_message_get_path(message);
    if (path == NULL || strcmp(path, client->notify_characteristic_path) != 0) {
        return;
    }

    DBusMessageIter iter;
    if (!dbus_message_iter_init(message, &iter)) {
        return;
    }

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        return;
    }

    const char *interface_name = NULL;
    dbus_message_iter_get_basic(&iter, &interface_name);
    if (interface_name == NULL || strcmp(interface_name, "org.bluez.GattCharacteristic1") != 0) {
        return;
    }

    if (!dbus_message_iter_next(&iter)) {
        return;
    }

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        return;
    }

    DBusMessageIter array_iter;
    dbus_message_iter_recurse(&iter, &array_iter);
    while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict_entry;
        dbus_message_iter_recurse(&array_iter, &dict_entry);

        if (dbus_message_iter_get_arg_type(&dict_entry) != DBUS_TYPE_STRING) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        const char *property_name = NULL;
        dbus_message_iter_get_basic(&dict_entry, &property_name);
        if (property_name == NULL) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        if (!dbus_message_iter_next(&dict_entry)) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        if (dbus_message_iter_get_arg_type(&dict_entry) != DBUS_TYPE_VARIANT) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        if (strcmp(property_name, "Value") != 0) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        DBusMessageIter variant_iter;
        dbus_message_iter_recurse(&dict_entry, &variant_iter);
        if (dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&variant_iter) != DBUS_TYPE_BYTE) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        /* get_fixed_array wants an iterator positioned inside the array, not on it. */
        DBusMessageIter bytes_iter;
        dbus_message_iter_recurse(&variant_iter, &bytes_iter);
        const uint8_t *payload = NULL;
        int length = 0;
        dbus_message_iter_get_fixed_array(&bytes_iter, &payload, &length);
        if (payload != NULL && length > 0 && client->notification_callback != NULL) {
            client->notification_callback(payload, (size_t)length, client->notification_userdata);
        }
        break;
    }
}

/* Answers one org.bluez.Agent1 call with no return values. */
static void mesh_bluez_agent_ack(struct mesh_bluez_client *client, DBusMessage *call) {
    DBusConnection *connection = (DBusConnection *)client->connection;
    DBusMessage *reply = dbus_message_new_method_return(call);
    if (reply == NULL || connection == NULL) {
        if (reply != NULL) {
            dbus_message_unref(reply);
        }
        return;
    }
    (void)dbus_connection_send(connection, reply, NULL);
    dbus_connection_flush(connection);
    dbus_message_unref(reply);
}

/* Holds an agent call that needs an answer from the user. The reply goes out later, from
   mesh_bluez_client_agent_submit_passkey() or _reject(); BlueZ blocks the pairing until then
   (its own request timeout is a minute, which is plenty to read a PIN off a node and type it). */
static void mesh_bluez_agent_defer(struct mesh_bluez_client *client, DBusMessage *call,
                                   enum mesh_bluez_agent_request_kind kind, const char *device_path,
                                   uint32_t passkey) {
    /* One at a time: a stale request can only be one BlueZ has already given up on. */
    if (client->agent_request.kind != MESH_BLUEZ_AGENT_REQUEST_NONE) {
        (void)mesh_bluez_client_agent_reject(client);
    }
    client->agent_pending_message = dbus_message_ref(call);
    client->agent_request.kind = kind;
    client->agent_request.passkey = passkey;
    snprintf(client->agent_request.device_path, sizeof(client->agent_request.device_path), "%s",
             device_path != NULL ? device_path : "");
}

/* org.bluez.Agent1, dispatched by hand: mesh_bluez_client_process() pops messages off the
   connection itself, so libdbus' object tree would never see them. */
static bool mesh_bluez_client_handle_agent_call(struct mesh_bluez_client *client,
                                                DBusMessage *message) {
    const char *path = dbus_message_get_path(message);
    if (path == NULL || strcmp(path, MESH_BLUEZ_AGENT_PATH) != 0) {
        return false;
    }

    const char *member = dbus_message_get_member(message);
    if (member == NULL) {
        return true;
    }

    if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        DBusConnection *connection = (DBusConnection *)client->connection;
        DBusMessage *reply = dbus_message_new_method_return(message);
        if (reply != NULL && connection != NULL) {
            const char *xml =
                "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
                "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
                "<node><interface name=\"org.bluez.Agent1\">"
                "<method name=\"Release\"/>"
                "<method name=\"RequestPinCode\"><arg type=\"o\" direction=\"in\"/>"
                "<arg type=\"s\" direction=\"out\"/></method>"
                "<method name=\"DisplayPinCode\"><arg type=\"o\" direction=\"in\"/>"
                "<arg type=\"s\" direction=\"in\"/></method>"
                "<method name=\"RequestPasskey\"><arg type=\"o\" direction=\"in\"/>"
                "<arg type=\"u\" direction=\"out\"/></method>"
                "<method name=\"DisplayPasskey\"><arg type=\"o\" direction=\"in\"/>"
                "<arg type=\"u\" direction=\"in\"/><arg type=\"q\" direction=\"in\"/></method>"
                "<method name=\"RequestConfirmation\"><arg type=\"o\" direction=\"in\"/>"
                "<arg type=\"u\" direction=\"in\"/></method>"
                "<method name=\"RequestAuthorization\"><arg type=\"o\" direction=\"in\"/></method>"
                "<method name=\"AuthorizeService\"><arg type=\"o\" direction=\"in\"/>"
                "<arg type=\"s\" direction=\"in\"/></method>"
                "<method name=\"Cancel\"/></interface></node>";
            dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
            (void)dbus_connection_send(connection, reply, NULL);
            dbus_connection_flush(connection);
        }
        if (reply != NULL) {
            dbus_message_unref(reply);
        }
        return true;
    }

    if (!dbus_message_has_interface(message, "org.bluez.Agent1")) {
        return true;
    }

    const char *device = NULL;
    dbus_uint32_t passkey = 0U;

    if (strcmp(member, "RequestPasskey") == 0) {
        dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
        mesh_log_info("bluez", "Pairing: %s is asking for its PIN", device != NULL ? device : "?");
        mesh_bluez_agent_defer(client, message, MESH_BLUEZ_AGENT_REQUEST_PASSKEY, device, 0U);
        return true;
    }
    if (strcmp(member, "RequestPinCode") == 0) {
        dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
        mesh_bluez_agent_defer(client, message, MESH_BLUEZ_AGENT_REQUEST_PINCODE, device, 0U);
        return true;
    }
    if (strcmp(member, "RequestConfirmation") == 0) {
        dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_UINT32,
                              &passkey, DBUS_TYPE_INVALID);
        /* Numeric comparison, which a node with no PIN set ends up in. The number is shown to
           the user rather than accepted blind: it is the only thing that says the bond is with
           the node in your hand and not something else that answered the pairing. */
        mesh_bluez_agent_defer(client, message, MESH_BLUEZ_AGENT_REQUEST_CONFIRM, device,
                               (uint32_t)passkey);
        return true;
    }
    if (strcmp(member, "DisplayPasskey") == 0 || strcmp(member, "DisplayPinCode") == 0) {
        /* The node is the one entering; nothing for us to do but say so in the log. */
        mesh_log_info("bluez", "Pairing: %s wants a code entered on it", member);
        mesh_bluez_agent_ack(client, message);
        return true;
    }
    if (strcmp(member, "RequestAuthorization") == 0 || strcmp(member, "AuthorizeService") == 0) {
        /*
         * Being the *default* agent means these also arrive for pairings someone else started
         * (a remote device pairing to the Brick) and for services on other devices entirely.
         * Acknowledging those would authorize them silently, so only the bond this client has
         * in flight is answered; anything else is refused.
         */
        /* The two carry different argument lists, and get_args fails on a mismatch - which
           would leave `device` NULL and refuse the legitimate case along with the rest. */
        if (strcmp(member, "AuthorizeService") == 0) {
            const char *uuid = NULL;
            dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_STRING,
                                  &uuid, DBUS_TYPE_INVALID);
        } else {
            dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
        }
        const bool ours = (client->pair_state == 1 && device != NULL &&
                           strcmp(device, client->pair_device_path) == 0);
        if (!ours) {
            mesh_log_warn("bluez", "Refusing %s for %s: no pairing of ours is in flight", member,
                          device != NULL ? device : "?");
            DBusConnection *connection = (DBusConnection *)client->connection;
            DBusMessage *reply =
                dbus_message_new_error(message, "org.bluez.Error.Rejected", "Not requested");
            if (reply != NULL) {
                if (connection != NULL) {
                    (void)dbus_connection_send(connection, reply, NULL);
                    dbus_connection_flush(connection);
                }
                dbus_message_unref(reply);
            }
            return true;
        }
        mesh_bluez_agent_ack(client, message);
        return true;
    }
    if (strcmp(member, "Cancel") == 0) {
        /* BlueZ gave up on the request; the held call must not be answered any more. */
        if (client->agent_pending_message != NULL) {
            dbus_message_unref((DBusMessage *)client->agent_pending_message);
            client->agent_pending_message = NULL;
        }
        memset(&client->agent_request, 0, sizeof(client->agent_request));
        mesh_log_warn("bluez", "Pairing request cancelled by BlueZ");
        mesh_bluez_agent_ack(client, message);
        return true;
    }
    if (strcmp(member, "Release") == 0) {
        client->agent_registered = false;
        mesh_bluez_agent_ack(client, message);
        return true;
    }

    mesh_bluez_agent_ack(client, message);
    return true;
}

static void mesh_bluez_client_handle_message(struct mesh_bluez_client *client,
                                             DBusMessage *message) {
    if (client == NULL || message == NULL) {
        return;
    }

    if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL &&
        mesh_bluez_client_handle_agent_call(client, message)) {
        return;
    }

    if (client->pair_state == 1 && client->pair_serial != 0U &&
        dbus_message_get_reply_serial(message) == client->pair_serial) {
        int type = dbus_message_get_type(message);
        if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
            client->pair_result = 0;
        } else {
            const char *error_name = dbus_message_get_error_name(message);
            char *text = NULL;
            dbus_message_get_args(message, NULL, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
            mesh_log_warn("bluez", "Pair failed: %s%s%s", error_name != NULL ? error_name : "?",
                          text != NULL ? ": " : "", text != NULL ? text : "");
            client->pair_result = mesh_bluez_error_to_errno(error_name, text);
        }
        client->pair_state = 2;
        return;
    }

    if (client->connect_state == 1 && client->connect_serial != 0U &&
        dbus_message_get_reply_serial(message) == client->connect_serial) {
        int type = dbus_message_get_type(message);
        if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
            client->connect_result = 0;
        } else {
            const char *error_name = dbus_message_get_error_name(message);
            char *text = NULL;
            dbus_message_get_args(message, NULL, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
            mesh_log_warn("bluez", "Connect failed: %s%s%s", error_name != NULL ? error_name : "?",
                          text != NULL ? ": " : "", text != NULL ? text : "");
            client->connect_result = mesh_bluez_error_to_errno(error_name, text);
        }
        client->connect_state = 2;
        return;
    }

    if (dbus_message_is_signal(message, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
        mesh_bluez_client_handle_properties_changed(client, message);
    }
}

static int mesh_bluez_client_add_properties_match(struct mesh_bluez_client *client,
                                                  const char *path) {
    if (client == NULL || path == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        return 0;
    }

    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    char rule[256];
    snprintf(rule, sizeof(rule),
             "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='"
             "PropertiesChanged',path='%s'",
             path);

    DBusError error;
    dbus_error_init(&error);
    dbus_bus_add_match(connection, rule, &error);
    if (dbus_error_is_set(&error)) {
        mesh_log_warn("bluez", "Failed to add match rule: %s", error.message);
        dbus_error_free(&error);
        return -EIO;
    }

    dbus_connection_flush(connection);
    return 0;
}

static void mesh_bluez_client_remove_properties_match(struct mesh_bluez_client *client,
                                                      const char *path) {
    if (client == NULL || path == NULL || path[0] == '\0') {
        return;
    }

    if (g_mock_state.enabled) {
        return;
    }

    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return;
    }

    char rule[256];
    snprintf(rule, sizeof(rule),
             "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='"
             "PropertiesChanged',path='%s'",
             path);

    dbus_bus_remove_match(connection, rule, NULL);
    dbus_connection_flush(connection);
}
#endif

int mesh_bluez_client_attach_loop(struct mesh_bluez_client *client, struct mesh_event_loop *loop) {
    if (client == NULL) {
        return -EINVAL;
    }

    client->loop = loop;

#ifdef MESH_HAVE_DBUS
    if (loop != NULL) {
        for (size_t i = 0; i < MESH_ARRAY_LEN(client->watches); ++i) {
            if (client->watches[i].watch != NULL) {
                mesh_bluez_watch_sync(client, i);
            }
        }
    }
#else
    (void)loop;
#endif

    return 0;
}

void mesh_bluez_client_detach_loop(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return;
    }

#ifdef MESH_HAVE_DBUS
    if (client->loop != NULL) {
        for (size_t i = 0; i < MESH_ARRAY_LEN(client->watches); ++i) {
            if (client->watches[i].registered) {
                mesh_event_loop_remove_fd(client->loop, client->watches[i].fd);
            }
            client->watches[i].registered = false;
        }
    }
#endif

    client->loop = NULL;
}

int mesh_bluez_client_process(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return -EINVAL;
    }

    if (!client->connected) {
        return -ENOTCONN;
    }

#ifdef MESH_HAVE_DBUS
    if (g_mock_state.enabled) {
        return 0;
    }

    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    dbus_connection_read_write(connection, 0);

    DBusMessage *message = NULL;
    while ((message = dbus_connection_pop_message(connection)) != NULL) {
        mesh_bluez_client_handle_message(client, message);
        dbus_message_unref(message);
    }

    return 0;
#else
    return 0;
#endif
}

void mesh_bluez_client_set_notification_handler(struct mesh_bluez_client *client,
                                                mesh_bluez_notification_callback callback,
                                                void *userdata) {
    if (client == NULL) {
        return;
    }

    client->notification_callback = callback;
    client->notification_userdata = userdata;
}

int mesh_bluez_client_find_adapter(struct mesh_bluez_client *client, char *path, size_t path_len) {
    if (client == NULL || path == NULL || path_len == 0U) {
        return -EINVAL;
    }

#ifdef MESH_HAVE_DBUS
    if (g_mock_state.enabled) {
        if (g_mock_state.config.find_adapter_result < 0) {
            return g_mock_state.config.find_adapter_result;
        }
        if (g_mock_state.config.adapter_path != NULL) {
            snprintf(path, path_len, "%s", g_mock_state.config.adapter_path);
        } else {
            snprintf(path, path_len, "%s", "/org/bluez/hci0");
        }
        return 0;
    }

    if (!client->connected || client->connection == NULL) {
        return -ENOTCONN;
    }

    DBusConnection *connection = (DBusConnection *)client->connection;

    DBusMessage *message = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(connection, message, 1000, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "GetManagedObjects failed: %s", error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }

    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -EIO;
    }

    bool found = false;
    DBusMessageIter array_iter = iter;
    dbus_message_iter_recurse(&iter, &array_iter);

    while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict_entry;
        dbus_message_iter_recurse(&array_iter, &dict_entry);

        if (dbus_message_iter_get_arg_type(&dict_entry) != DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        const char *object_path = NULL;
        dbus_message_iter_get_basic(&dict_entry, &object_path);
        dbus_message_iter_next(&dict_entry);

        if (dbus_message_iter_get_arg_type(&dict_entry) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        DBusMessageIter iface_iter;
        dbus_message_iter_recurse(&dict_entry, &iface_iter);
        while (dbus_message_iter_get_arg_type(&iface_iter) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter iface_entry;
            dbus_message_iter_recurse(&iface_iter, &iface_entry);

            if (dbus_message_iter_get_arg_type(&iface_entry) != DBUS_TYPE_STRING) {
                dbus_message_iter_next(&iface_iter);
                continue;
            }

            const char *interface_name = NULL;
            dbus_message_iter_get_basic(&iface_entry, &interface_name);
            if (interface_name != NULL && strcmp(interface_name, "org.bluez.Adapter1") == 0) {
                snprintf(path, path_len, "%s", object_path);
                found = true;
                break;
            }

            dbus_message_iter_next(&iface_entry);
            dbus_message_iter_next(&iface_iter);
        }

        if (found) {
            break;
        }

        dbus_message_iter_next(&array_iter);
    }

    dbus_message_unref(reply);
    return found ? 0 : -ENODEV;
#else
    if (g_mock_state.enabled) {
        if (g_mock_state.config.find_adapter_result < 0) {
            return g_mock_state.config.find_adapter_result;
        }
        if (g_mock_state.config.adapter_path != NULL) {
            snprintf(path, path_len, "%s", g_mock_state.config.adapter_path);
        } else {
            snprintf(path, path_len, "%s", "/org/bluez/hci0");
        }
        return 0;
    }
    (void)client;
    (void)path;
    (void)path_len;
    return -ENOSYS;
#endif
}

static int call_adapter_method(struct mesh_bluez_client *client, const char *adapter_path,
                               const char *method) {
#ifdef MESH_HAVE_DBUS
    if (client == NULL || adapter_path == NULL || method == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        if (strcmp(method, "StartDiscovery") == 0) {
            return g_mock_state.config.start_discovery_result;
        }
        if (strcmp(method, "StopDiscovery") == 0) {
            return g_mock_state.config.stop_discovery_result;
        }
        return 0;
    }

    if (!client->connected || client->connection == NULL) {
        return -ENOTCONN;
    }

    DBusConnection *connection = (DBusConnection *)client->connection;
    DBusMessage *message =
        dbus_message_new_method_call("org.bluez", adapter_path, "org.bluez.Adapter1", method);
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        connection, message, DBUS_TIMEOUT_USE_DEFAULT, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "%s failed: %s", method, error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }

    dbus_message_unref(reply);
    return 0;
#else
    if (client == NULL || adapter_path == NULL || method == NULL) {
        return -EINVAL;
    }
    if (g_mock_state.enabled) {
        if (strcmp(method, "StartDiscovery") == 0) {
            return g_mock_state.config.start_discovery_result;
        }
        if (strcmp(method, "StopDiscovery") == 0) {
            return g_mock_state.config.stop_discovery_result;
        }
        return 0;
    }
    (void)client;
    (void)adapter_path;
    (void)method;
    return -ENOSYS;
#endif
}

int mesh_bluez_client_start_discovery(struct mesh_bluez_client *client, const char *adapter_path) {
    return call_adapter_method(client, adapter_path, "StartDiscovery");
}

int mesh_bluez_client_stop_discovery(struct mesh_bluez_client *client, const char *adapter_path) {
    return call_adapter_method(client, adapter_path, "StopDiscovery");
}

int mesh_bluez_client_connect_begin(struct mesh_bluez_client *client, const char *device_path) {
    if (client == NULL || device_path == NULL) {
        return -EINVAL;
    }
    if (client->connect_state == 1) {
        return -EBUSY;
    }

    if (g_mock_state.enabled) {
        g_mock_state.connect_polls = 0U;
        client->connect_state = 1;
        client->connect_serial = 1U;
        client->connect_result = g_mock_state.config.connect_result;
        return 0;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage *message =
        dbus_message_new_method_call("org.bluez", device_path, "org.bluez.Device1", "Connect");
    if (message == NULL) {
        return -ENOMEM;
    }

    dbus_uint32_t serial = 0U;
    dbus_bool_t sent = dbus_connection_send(connection, message, &serial);
    dbus_message_unref(message);
    if (!sent) {
        return -EIO;
    }
    dbus_connection_flush(connection);

    client->connect_serial = serial;
    client->connect_state = 1;
    client->connect_result = 0;
    return 0;
#else
    (void)client;
    (void)device_path;
    return -ENOSYS;
#endif
}

int mesh_bluez_client_connect_poll(struct mesh_bluez_client *client, int *out_result) {
    if (client == NULL || out_result == NULL) {
        return -EINVAL;
    }
    if (client->connect_state == 0) {
        return -EINVAL;
    }

    if (g_mock_state.enabled && client->connect_state == 1) {
        g_mock_state.connect_polls++;
        if (g_mock_state.connect_polls > g_mock_state.config.connect_pending_polls) {
            client->connect_state = 2;
            if (client->connect_result == 0) {
                g_mock_state.client = client;
            }
        }
    }

    if (client->connect_state != 2) {
        return 0;
    }

    *out_result = client->connect_result;
    client->connect_state = 0;
    client->connect_serial = 0U;
    return 1;
}

void mesh_bluez_client_connect_cancel(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return;
    }
    client->connect_state = 0;
    client->connect_serial = 0U;
    client->connect_result = 0;
}

/* ---- pairing -------------------------------------------------------------------------------
 *
 * Everything a node in PIN mode needs, so the user never has to leave the app for bluetoothctl.
 * Two halves that only make sense together: Device1.Pair, sent without blocking because it does
 * not answer until the bond is done, and an org.bluez.Agent1 that BlueZ calls back on to ask for
 * the six digits the node is showing. The agent's reply is deferred - we hold the call message
 * until the user has typed them - which is why the pair can outlive several event-loop turns.
 */

int mesh_bluez_client_pair_begin(struct mesh_bluez_client *client, const char *device_path) {
    if (client == NULL || device_path == NULL) {
        return -EINVAL;
    }
    if (client->pair_state == 1) {
        return -EBUSY;
    }

    snprintf(client->pair_device_path, sizeof(client->pair_device_path), "%s", device_path);

    if (g_mock_state.enabled) {
        g_mock_state.pair_polls = 0U;
        client->pair_state = 1;
        client->pair_serial = 1U;
        client->pair_result = g_mock_state.config.pair_result;
        if (g_mock_state.config.pair_requests_passkey) {
            client->agent_request.kind = MESH_BLUEZ_AGENT_REQUEST_PASSKEY;
            snprintf(client->agent_request.device_path, sizeof(client->agent_request.device_path),
                     "%s", device_path);
            client->agent_request.passkey = 0U;
        }
        return 0;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage *message =
        dbus_message_new_method_call("org.bluez", device_path, "org.bluez.Device1", "Pair");
    if (message == NULL) {
        return -ENOMEM;
    }

    dbus_uint32_t serial = 0U;
    dbus_bool_t sent = dbus_connection_send(connection, message, &serial);
    dbus_message_unref(message);
    if (!sent) {
        return -EIO;
    }
    dbus_connection_flush(connection);

    client->pair_serial = serial;
    client->pair_state = 1;
    client->pair_result = 0;
    mesh_log_info("bluez", "Pairing with %s", device_path);
    return 0;
#else
    return -ENOSYS;
#endif
}

int mesh_bluez_client_pair_poll(struct mesh_bluez_client *client, int *out_result) {
    if (client == NULL || out_result == NULL) {
        return -EINVAL;
    }
    if (client->pair_state == 0) {
        return -EINVAL;
    }

    if (g_mock_state.enabled && client->pair_state == 1) {
        /* BlueZ does not answer Pair while its agent is waiting on the user, and neither does
           the mock: a test that never submits a passkey sees the pair stay pending. */
        if (client->agent_request.kind == MESH_BLUEZ_AGENT_REQUEST_NONE) {
            g_mock_state.pair_polls++;
            if (g_mock_state.pair_polls > g_mock_state.config.pair_pending_polls) {
                client->pair_state = 2;
                if (client->pair_result == 0) {
                    /* BlueZ would now report the device Paired; so does the device list. */
                    mesh_bluez_mock_note_paired(client->pair_device_path);
                }
            }
        }
    }

    if (client->pair_state != 2) {
        return 0;
    }

    *out_result = client->pair_result;
    client->pair_state = 0;
    client->pair_serial = 0U;
    return 1;
}

void mesh_bluez_client_pair_cancel(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return;
    }

    (void)mesh_bluez_client_agent_reject(client);

#ifdef MESH_HAVE_DBUS
    if (!g_mock_state.enabled && client->pair_state == 1 && client->pair_device_path[0] != '\0') {
        DBusConnection *connection = (DBusConnection *)client->connection;
        if (connection != NULL) {
            DBusMessage *message = dbus_message_new_method_call(
                "org.bluez", client->pair_device_path, "org.bluez.Device1", "CancelPairing");
            if (message != NULL) {
                (void)dbus_connection_send(connection, message, NULL);
                dbus_message_unref(message);
                dbus_connection_flush(connection);
            }
        }
    }
#endif

    client->pair_state = 0;
    client->pair_serial = 0U;
    client->pair_result = 0;
    client->pair_device_path[0] = '\0';
}

int mesh_bluez_client_set_trusted(struct mesh_bluez_client *client, const char *device_path,
                                  bool trusted) {
    if (client == NULL || device_path == NULL) {
        return -EINVAL;
    }
    if (g_mock_state.enabled) {
        return 0;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage *message = dbus_message_new_method_call("org.bluez", device_path,
                                                        "org.freedesktop.DBus.Properties", "Set");
    if (message == NULL) {
        return -ENOMEM;
    }

    const char *interface = "org.bluez.Device1";
    const char *property = "Trusted";
    dbus_bool_t value = trusted ? TRUE : FALSE;

    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &property);
    DBusMessageIter variant;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, DBUS_TYPE_BOOLEAN_AS_STRING,
                                     &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
    dbus_message_iter_close_container(&iter, &variant);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(connection, message, 2000, &error);
    dbus_message_unref(message);
    if (reply == NULL) {
        int mapped = -EIO;
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "Set Trusted failed: %s", error.message);
            mapped = mesh_bluez_dbus_error_to_errno(&error);
            dbus_error_free(&error);
        }
        return mapped;
    }
    dbus_message_unref(reply);
    return 0;
#else
    (void)trusted;
    return -ENOSYS;
#endif
}

int mesh_bluez_client_remove_device(struct mesh_bluez_client *client, const char *adapter_path,
                                    const char *device_path) {
    if (client == NULL || adapter_path == NULL || device_path == NULL) {
        return -EINVAL;
    }
    if (g_mock_state.enabled) {
        return g_mock_state.config.remove_device_result;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage *message = dbus_message_new_method_call("org.bluez", adapter_path,
                                                        "org.bluez.Adapter1", "RemoveDevice");
    if (message == NULL) {
        return -ENOMEM;
    }
    dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &device_path, DBUS_TYPE_INVALID);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(connection, message, 5000, &error);
    dbus_message_unref(message);
    if (reply == NULL) {
        int mapped = -EIO;
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "RemoveDevice failed: %s", error.message);
            mapped = mesh_bluez_dbus_error_to_errno(&error);
            dbus_error_free(&error);
        }
        return mapped;
    }
    dbus_message_unref(reply);
    mesh_log_info("bluez", "Removed %s", device_path);
    return 0;
#else
    return -ENOSYS;
#endif
}

/* ---- pairing agent -------------------------------------------------------------------------- */

int mesh_bluez_client_register_agent(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return -EINVAL;
    }
    if (client->agent_registered) {
        return 0;
    }
    if (g_mock_state.enabled) {
        client->agent_registered = true;
        return 0;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage *message = dbus_message_new_method_call("org.bluez", "/org/bluez",
                                                        "org.bluez.AgentManager1", "RegisterAgent");
    if (message == NULL) {
        return -ENOMEM;
    }

    const char *path = MESH_BLUEZ_AGENT_PATH;
    /* KeyboardDisplay is what makes a PIN-mode node choose passkey entry: its own capability is
       DisplayOnly, so BlueZ asks us for the number rather than the other way round. */
    const char *capability = "KeyboardDisplay";
    dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_STRING, &capability,
                             DBUS_TYPE_INVALID);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(connection, message, 2000, &error);
    dbus_message_unref(message);
    if (reply == NULL) {
        int mapped = -EIO;
        if (dbus_error_is_set(&error)) {
            /* An agent left behind by a previous run of this same process name is the common
               case; treat it as registered rather than losing pairing for the session. */
            const bool exists =
                (error.name != NULL && strcmp(error.name, "org.bluez.Error.AlreadyExists") == 0);
            if (exists) {
                dbus_error_free(&error);
                client->agent_registered = true;
                return 0;
            }
            mesh_log_warn("bluez", "RegisterAgent failed: %s", error.message);
            mapped = mesh_bluez_dbus_error_to_errno(&error);
            dbus_error_free(&error);
        }
        return mapped;
    }
    dbus_message_unref(reply);

    /* Being the default agent is what routes requests here on a device with no bluetoothctl
       running. It is not fatal if something else already claimed it. */
    DBusMessage *request = dbus_message_new_method_call(
        "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent");
    if (request != NULL) {
        dbus_message_append_args(request, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
        dbus_error_init(&error);
        DBusMessage *default_reply =
            dbus_connection_send_with_reply_and_block(connection, request, 2000, &error);
        dbus_message_unref(request);
        if (default_reply != NULL) {
            dbus_message_unref(default_reply);
        } else if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "RequestDefaultAgent failed: %s", error.message);
            dbus_error_free(&error);
        }
    }

    client->agent_registered = true;
    mesh_log_info("bluez", "Pairing agent registered at %s", path);
    return 0;
#else
    return -ENOSYS;
#endif
}

void mesh_bluez_client_unregister_agent(struct mesh_bluez_client *client) {
    if (client == NULL || !client->agent_registered) {
        return;
    }
    (void)mesh_bluez_client_agent_reject(client);
    client->agent_registered = false;

    if (g_mock_state.enabled) {
        return;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return;
    }
    DBusMessage *message = dbus_message_new_method_call(
        "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent");
    if (message == NULL) {
        return;
    }
    const char *path = MESH_BLUEZ_AGENT_PATH;
    dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
    (void)dbus_connection_send(connection, message, NULL);
    dbus_message_unref(message);
    dbus_connection_flush(connection);
#endif
}

bool mesh_bluez_client_agent_request(const struct mesh_bluez_client *client,
                                     struct mesh_bluez_agent_request *out) {
    if (client == NULL || client->agent_request.kind == MESH_BLUEZ_AGENT_REQUEST_NONE) {
        return false;
    }
    if (out != NULL) {
        *out = client->agent_request;
    }
    return true;
}

/* Sends the reply BlueZ is blocked on and forgets the request. `reply` is consumed. */
static void mesh_bluez_agent_finish(struct mesh_bluez_client *client, void *reply) {
#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (reply != NULL) {
        if (connection != NULL) {
            (void)dbus_connection_send(connection, (DBusMessage *)reply, NULL);
            dbus_connection_flush(connection);
        }
        dbus_message_unref((DBusMessage *)reply);
    }
    if (client->agent_pending_message != NULL) {
        dbus_message_unref((DBusMessage *)client->agent_pending_message);
    }
#else
    (void)reply;
#endif
    client->agent_pending_message = NULL;
    memset(&client->agent_request, 0, sizeof(client->agent_request));
}

int mesh_bluez_client_agent_submit_passkey(struct mesh_bluez_client *client, uint32_t passkey) {
    if (client == NULL) {
        return -EINVAL;
    }
    const enum mesh_bluez_agent_request_kind kind = client->agent_request.kind;
    if (kind == MESH_BLUEZ_AGENT_REQUEST_NONE) {
        return -ENOENT;
    }

    if (g_mock_state.enabled) {
        if (g_mock_state.config.pair_passkey_capture != NULL) {
            *g_mock_state.config.pair_passkey_capture = passkey;
        }
        mesh_bluez_agent_finish(client, NULL);
        return 0;
    }

#ifdef MESH_HAVE_DBUS
    DBusMessage *call = (DBusMessage *)client->agent_pending_message;
    if (call == NULL) {
        memset(&client->agent_request, 0, sizeof(client->agent_request));
        return -ENOENT;
    }
    DBusMessage *reply = dbus_message_new_method_return(call);
    if (reply == NULL) {
        return -ENOMEM;
    }
    if (kind == MESH_BLUEZ_AGENT_REQUEST_PASSKEY) {
        dbus_uint32_t value = (dbus_uint32_t)passkey;
        dbus_message_append_args(reply, DBUS_TYPE_UINT32, &value, DBUS_TYPE_INVALID);
    } else if (kind == MESH_BLUEZ_AGENT_REQUEST_PINCODE) {
        /* The legacy PIN is a string, and Meshtastic's is always the six digits it displays. */
        char digits[16];
        snprintf(digits, sizeof(digits), "%06u", (unsigned)passkey);
        const char *text = digits;
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
    }
    /* CONFIRM takes an empty reply: sending it *is* the confirmation. */
    mesh_log_info("bluez", "Answered pairing request for %s", client->agent_request.device_path);
    mesh_bluez_agent_finish(client, reply);
    return 0;
#else
    (void)passkey;
    return -ENOSYS;
#endif
}

int mesh_bluez_client_agent_confirm(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return -EINVAL;
    }
    if (client->agent_request.kind != MESH_BLUEZ_AGENT_REQUEST_CONFIRM) {
        return -ENOENT;
    }
    return mesh_bluez_client_agent_submit_passkey(client, client->agent_request.passkey);
}

int mesh_bluez_client_agent_reject(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return -EINVAL;
    }
    if (client->agent_request.kind == MESH_BLUEZ_AGENT_REQUEST_NONE) {
        return -ENOENT;
    }

    if (g_mock_state.enabled) {
        /* Refusing the question is refusing the bond, which is what BlueZ does with it. */
        if (client->pair_state == 1) {
            client->pair_result = -EACCES;
        }
        mesh_bluez_agent_finish(client, NULL);
        return 0;
    }

#ifdef MESH_HAVE_DBUS
    DBusMessage *call = (DBusMessage *)client->agent_pending_message;
    DBusMessage *reply = (call != NULL)
                             ? dbus_message_new_error(call, "org.bluez.Error.Rejected", "Cancelled")
                             : NULL;
    mesh_bluez_agent_finish(client, reply);
    return 0;
#else
    return -ENOSYS;
#endif
}

#ifdef MESH_HAVE_DBUS
/* Properties.Get on org.bluez.Device1 for a boolean property, with a short timeout: this is
   polled from the loop, so it must never sit on a 25 s default. */
static int mesh_bluez_get_device_boolean(struct mesh_bluez_client *client, const char *device_path,
                                         const char *property, bool *out_value) {
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage *message = dbus_message_new_method_call("org.bluez", device_path,
                                                        "org.freedesktop.DBus.Properties", "Get");
    if (message == NULL) {
        return -ENOMEM;
    }

    const char *interface = "org.bluez.Device1";
    if (!dbus_message_append_args(message, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING,
                                  &property, DBUS_TYPE_INVALID)) {
        dbus_message_unref(message);
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        connection, message, MESH_BLUEZ_PROPERTY_TIMEOUT_MS, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "Get %s failed: %s", property, error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }

    int result = -EIO;
    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        DBusMessageIter variant_iter;
        dbus_message_iter_recurse(&iter, &variant_iter);
        if (dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t value = FALSE;
            dbus_message_iter_get_basic(&variant_iter, &value);
            *out_value = value ? true : false;
            result = 0;
        }
    }
    dbus_message_unref(reply);
    return result;
}
#endif

int mesh_bluez_client_services_resolved(struct mesh_bluez_client *client, const char *device_path,
                                        bool *out_resolved) {
    if (client == NULL || device_path == NULL || out_resolved == NULL) {
        return -EINVAL;
    }

    *out_resolved = false;

    if (g_mock_state.enabled) {
        if (g_mock_state.config.services_resolved_result != 0) {
            return g_mock_state.config.services_resolved_result;
        }
        g_mock_state.services_resolved_polls++;
        *out_resolved = g_mock_state.services_resolved_polls >
                        g_mock_state.config.services_resolved_after_polls;
        return 0;
    }

#ifdef MESH_HAVE_DBUS
    return mesh_bluez_get_device_boolean(client, device_path, "ServicesResolved", out_resolved);
#else
    return -ENOSYS;
#endif
}

int mesh_bluez_client_device_connected(struct mesh_bluez_client *client, const char *device_path,
                                       bool *out_connected) {
    if (client == NULL || device_path == NULL || out_connected == NULL) {
        return -EINVAL;
    }

    *out_connected = false;

    if (g_mock_state.enabled) {
        g_mock_state.connected_polls++;
        *out_connected =
            g_mock_state.config.connected_drops_after_polls == 0U ||
            g_mock_state.connected_polls <= g_mock_state.config.connected_drops_after_polls;
        return 0;
    }

#ifdef MESH_HAVE_DBUS
    return mesh_bluez_get_device_boolean(client, device_path, "Connected", out_connected);
#else
    return -ENOSYS;
#endif
}

int mesh_bluez_client_disconnect(struct mesh_bluez_client *client, const char *device_path) {
    if (client == NULL || device_path == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        int result = g_mock_state.config.disconnect_result;
        if (result == 0 && g_mock_state.client == client) {
            g_mock_state.client = NULL;
            client->notify_characteristic_path[0] = '\0';
        }
        return result;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage *message =
        dbus_message_new_method_call("org.bluez", device_path, "org.bluez.Device1", "Disconnect");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        connection, message, DBUS_TIMEOUT_USE_DEFAULT, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "Disconnect failed: %s", error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }

    dbus_message_unref(reply);

    mesh_bluez_client_remove_properties_match(client, client->notify_characteristic_path);
    client->notify_characteristic_path[0] = '\0';
    return 0;
#else
    (void)client;
    (void)device_path;
    return -ENOSYS;
#endif
}

int mesh_bluez_client_subscribe(struct mesh_bluez_client *client, const char *device_path,
                                const char *char_uuid) {
    if (client == NULL || device_path == NULL || char_uuid == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        if (g_mock_state.config.subscribe_result == 0) {
            snprintf(client->notify_characteristic_path, sizeof(client->notify_characteristic_path),
                     "%s", device_path);
        }
        return g_mock_state.config.subscribe_result;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage *message = dbus_message_new_method_call(
        "org.bluez", device_path, "org.bluez.GattCharacteristic1", "StartNotify");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    /*
     * Deliberately not the 25 s default. StartNotify on an encrypted characteristic can make
     * BlueZ start a pairing, and BlueZ then calls our agent - which we cannot answer from
     * inside a blocking call, because the loop that pops messages is this thread. Bonding is
     * done up front (mesh_ble_transport_pair) precisely so this does not happen; the timeout
     * bounds the stall for the case it still can, a bond that has gone stale on the node.
     */
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(connection, message, 8000, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        int mapped = -EIO;
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "StartNotify failed: %s", error.message);
            mapped = mesh_bluez_dbus_error_to_errno(&error);
            dbus_error_free(&error);
        }
        return mapped;
    }

    dbus_message_unref(reply);

    snprintf(client->notify_characteristic_path, sizeof(client->notify_characteristic_path), "%s",
             device_path);
    int match_result =
        mesh_bluez_client_add_properties_match(client, client->notify_characteristic_path);
    if (match_result < 0) {
        mesh_log_warn("bluez", "Failed to add notification match for %s",
                      client->notify_characteristic_path);
    }
    return 0;
#else
    (void)client;
    (void)device_path;
    (void)char_uuid;
    return -ENOSYS;
#endif
}

int mesh_bluez_client_write(struct mesh_bluez_client *client, const char *device_path,
                            const char *char_uuid, const uint8_t *data, size_t len) {
    if (client == NULL || device_path == NULL || char_uuid == NULL || data == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        size_t call_index = 0U;
        if (g_mock_state.config.write_call_count != NULL) {
            (*g_mock_state.config.write_call_count)++;
            call_index = *g_mock_state.config.write_call_count;
        }

        if (g_mock_state.config.write_lengths != NULL && call_index > 0U &&
            (call_index - 1U) < g_mock_state.config.write_lengths_capacity) {
            g_mock_state.config.write_lengths[call_index - 1U] = len;
        }

        if (g_mock_state.config.write_capture_buffer != NULL &&
            g_mock_state.config.write_capture_length != NULL) {
            size_t to_copy = len;
            if (to_copy > g_mock_state.config.write_capture_capacity) {
                to_copy = g_mock_state.config.write_capture_capacity;
            }
            if (to_copy > 0U) {
                memcpy(g_mock_state.config.write_capture_buffer, data, to_copy);
            }
            *g_mock_state.config.write_capture_length = to_copy;
        }

        if (g_mock_state.config.write_capture_path != NULL &&
            g_mock_state.config.write_capture_path_capacity > 0U) {
            snprintf(g_mock_state.config.write_capture_path,
                     g_mock_state.config.write_capture_path_capacity, "%s",
                     device_path != NULL ? device_path : "");
        }

        g_mock_state.write_calls++;
        if (g_mock_state.config.write_fail_after_calls != 0U &&
            g_mock_state.write_calls > g_mock_state.config.write_fail_after_calls) {
            return g_mock_state.config.write_result_late;
        }
        return g_mock_state.config.write_result;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    DBusMessage *message = dbus_message_new_method_call(
        "org.bluez", device_path, "org.bluez.GattCharacteristic1", "WriteValue");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);

    DBusMessageIter array_iter;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "y", &array_iter);
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        if (!dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_BYTE, &byte)) {
            dbus_message_iter_close_container(&iter, &array_iter);
            dbus_message_unref(message);
            return -ENOMEM;
        }
    }
    dbus_message_iter_close_container(&iter, &array_iter);

    DBusMessageIter options_iter;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options_iter);
    dbus_message_iter_close_container(&iter, &options_iter);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        connection, message, DBUS_TIMEOUT_USE_DEFAULT, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "WriteValue failed: %s", error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }

    dbus_message_unref(reply);
    return 0;
#else
    (void)client;
    (void)device_path;
    (void)char_uuid;
    (void)data;
    (void)len;
    return -ENOSYS;
#endif
}

#ifdef MESH_HAVE_DBUS
static int mesh_bluez_find_characteristics(DBusConnection *connection, const char *device_path,
                                           const char *uuid, char *out_path, size_t out_len) {
    DBusMessage *message = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(connection, message, 1000, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "GetManagedObjects failed: %s", error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }

    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -EIO;
    }

    DBusMessageIter array_iter;
    dbus_message_iter_recurse(&iter, &array_iter);
    bool found = false;

    while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict_entry;
        dbus_message_iter_recurse(&array_iter, &dict_entry);
        if (dbus_message_iter_get_arg_type(&dict_entry) != DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        const char *object_path = NULL;
        dbus_message_iter_get_basic(&dict_entry, &object_path);
        dbus_message_iter_next(&dict_entry);

        if (object_path == NULL || strncmp(object_path, device_path, strlen(device_path)) != 0) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        if (dbus_message_iter_get_arg_type(&dict_entry) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        DBusMessageIter iface_iter;
        dbus_message_iter_recurse(&dict_entry, &iface_iter);
        while (dbus_message_iter_get_arg_type(&iface_iter) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter iface_entry;
            dbus_message_iter_recurse(&iface_iter, &iface_entry);
            if (dbus_message_iter_get_arg_type(&iface_entry) != DBUS_TYPE_STRING) {
                dbus_message_iter_next(&iface_iter);
                continue;
            }

            const char *interface_name = NULL;
            dbus_message_iter_get_basic(&iface_entry, &interface_name);
            dbus_message_iter_next(&iface_entry);

            if (interface_name == NULL ||
                strcmp(interface_name, "org.bluez.GattCharacteristic1") != 0) {
                dbus_message_iter_next(&iface_iter);
                continue;
            }

            if (dbus_message_iter_get_arg_type(&iface_entry) != DBUS_TYPE_ARRAY) {
                dbus_message_iter_next(&iface_iter);
                continue;
            }

            DBusMessageIter props_iter;
            dbus_message_iter_recurse(&iface_entry, &props_iter);
            while (dbus_message_iter_get_arg_type(&props_iter) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter prop_entry;
                dbus_message_iter_recurse(&props_iter, &prop_entry);
                if (dbus_message_iter_get_arg_type(&prop_entry) != DBUS_TYPE_STRING) {
                    dbus_message_iter_next(&props_iter);
                    continue;
                }

                const char *property_name = NULL;
                dbus_message_iter_get_basic(&prop_entry, &property_name);
                dbus_message_iter_next(&prop_entry);

                if (strcmp(property_name, "UUID") == 0) {
                    DBusMessageIter variant_iter;
                    dbus_message_iter_recurse(&prop_entry, &variant_iter);
                    if (dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_STRING) {
                        const char *value = NULL;
                        dbus_message_iter_get_basic(&variant_iter, &value);
                        if (value != NULL && strcasecmp(value, uuid) == 0) {
                            snprintf(out_path, out_len, "%s", object_path);
                            found = true;
                            break;
                        }
                    }
                }

                dbus_message_iter_next(&props_iter);
            }

            if (found) {
                break;
            }

            dbus_message_iter_next(&iface_iter);
        }

        if (found) {
            break;
        }

        dbus_message_iter_next(&array_iter);
    }

    dbus_message_unref(reply);
    return found ? 0 : -ENOENT;
}
#endif

int mesh_bluez_client_find_meshtastic_characteristics(struct mesh_bluez_client *client,
                                                      const char *device_path,
                                                      struct mesh_bluez_meshtastic_chars *out) {
    if (client == NULL || device_path == NULL || out == NULL) {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));

    if (g_mock_state.enabled) {
        const struct mesh_bluez_mock_config *cfg = &g_mock_state.config;
        snprintf(out->toradio_path, sizeof(out->toradio_path), "%s",
                 cfg->toradio_char_path != NULL ? cfg->toradio_char_path : device_path);
        if (cfg->toradio_char_path == NULL) {
            snprintf(out->toradio_path, sizeof(out->toradio_path), "%s/toradio", device_path);
        }
        if (cfg->fromradio_char_path != NULL) {
            snprintf(out->fromradio_path, sizeof(out->fromradio_path), "%s",
                     cfg->fromradio_char_path);
        } else {
            snprintf(out->fromradio_path, sizeof(out->fromradio_path), "%s/fromradio", device_path);
        }
        if (cfg->fromnum_char_path != NULL) {
            snprintf(out->fromnum_path, sizeof(out->fromnum_path), "%s", cfg->fromnum_char_path);
        } else {
            snprintf(out->fromnum_path, sizeof(out->fromnum_path), "%s/fromnum", device_path);
        }
        return 0;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    int result = mesh_bluez_find_characteristics(connection, device_path, MESH_BLE_TORADIO_UUID,
                                                 out->toradio_path, sizeof(out->toradio_path));
    if (result < 0) {
        mesh_log_warn("bluez", "ToRadio characteristic not found under %s", device_path);
        return result;
    }
    result = mesh_bluez_find_characteristics(connection, device_path, MESH_BLE_FROMRADIO_UUID,
                                             out->fromradio_path, sizeof(out->fromradio_path));
    if (result < 0) {
        mesh_log_warn("bluez", "FromRadio characteristic not found under %s", device_path);
        return result;
    }
    result = mesh_bluez_find_characteristics(connection, device_path, MESH_BLE_FROMNUM_UUID,
                                             out->fromnum_path, sizeof(out->fromnum_path));
    if (result < 0) {
        mesh_log_warn("bluez", "FromNum characteristic not found under %s", device_path);
        return result;
    }
    /* LogRadio is optional; older firmware does not expose it. */
    if (mesh_bluez_find_characteristics(connection, device_path, MESH_BLE_LOGRADIO_UUID,
                                        out->logradio_path, sizeof(out->logradio_path)) < 0) {
        out->logradio_path[0] = '\0';
    }
    return 0;
#else
    (void)client;
    (void)device_path;
    (void)out;
    return -ENOSYS;
#endif
}

int mesh_bluez_client_read(struct mesh_bluez_client *client, const char *char_path, uint8_t *out,
                           size_t capacity, size_t *out_len) {
    if (client == NULL || char_path == NULL || out == NULL || out_len == NULL) {
        return -EINVAL;
    }

    *out_len = 0U;

    if (g_mock_state.enabled) {
        const struct mesh_bluez_mock_config *cfg = &g_mock_state.config;
        if (cfg->read_result != 0) {
            return cfg->read_result;
        }
        size_t index = cfg->read_index != NULL ? *cfg->read_index : g_mock_state.read_cursor;
        if (cfg->read_payloads != NULL && cfg->read_payload_lengths != NULL &&
            index < cfg->read_payload_count) {
            size_t len = cfg->read_payload_lengths[index];
            if (len > capacity) {
                len = capacity;
            }
            memcpy(out, cfg->read_payloads[index], len);
            *out_len = len;
        }
        index++;
        if (cfg->read_index != NULL) {
            *cfg->read_index = index;
        } else {
            g_mock_state.read_cursor = index;
        }
        return 0;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage *message = dbus_message_new_method_call(
        "org.bluez", char_path, "org.bluez.GattCharacteristic1", "ReadValue");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    DBusMessageIter options_iter;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options_iter);
    dbus_message_iter_close_container(&iter, &options_iter);

    /* A GATT read normally completes in tens of ms; never sit on libdbus's 25 s default. */
    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        connection, message, MESH_BLUEZ_READ_TIMEOUT_MS, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "ReadValue failed: %s", error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }

    DBusMessageIter reply_iter;
    if (!dbus_message_iter_init(reply, &reply_iter) ||
        dbus_message_iter_get_arg_type(&reply_iter) != DBUS_TYPE_ARRAY ||
        dbus_message_iter_get_element_type(&reply_iter) != DBUS_TYPE_BYTE) {
        dbus_message_unref(reply);
        return -EPROTO;
    }

    DBusMessageIter array_iter;
    dbus_message_iter_recurse(&reply_iter, &array_iter);
    const uint8_t *payload = NULL;
    int length = 0;
    dbus_message_iter_get_fixed_array(&array_iter, &payload, &length);

    int result = 0;
    if (length > 0 && payload != NULL) {
        if ((size_t)length > capacity) {
            mesh_log_warn("bluez", "ReadValue returned %d bytes, buffer holds %zu", length,
                          capacity);
            result = -EMSGSIZE;
        } else {
            memcpy(out, payload, (size_t)length);
            *out_len = (size_t)length;
        }
    }

    dbus_message_unref(reply);
    return result;
#else
    (void)client;
    (void)char_path;
    (void)out;
    (void)capacity;
    return -ENOSYS;
#endif
}

#ifdef MESH_HAVE_DBUS
static bool uuid_equals_meshtastic(const char *uuid) {
    if (uuid == NULL) {
        return false;
    }
    char buffer[37];
    size_t index = 0;
    for (const char *c = uuid; *c != '\0' && index < sizeof(buffer) - 1; ++c) {
        buffer[index++] = (char)toupper((unsigned char)*c);
    }
    buffer[index] = '\0';
    return strcmp(buffer, MESH_BLE_MESHTASTIC_SERVICE_UUID) == 0;
}
#endif

int mesh_bluez_client_list_meshtastic(struct mesh_bluez_client *client,
                                      struct mesh_bluez_device_info *devices, size_t capacity,
                                      size_t *count) {
    if (client == NULL || devices == NULL || count == NULL) {
        return -EINVAL;
    }

    *count = 0;
    if (capacity == 0U) {
        return 0;
    }

    if (!client->connected) {
        return -ENOTCONN;
    }

    if (g_mock_state.enabled) {
        mesh_bluez_apply_mock_devices(devices, capacity, count);
        return g_mock_state.config.list_result;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    DBusMessage *message = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(connection, message, 1000, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "GetManagedObjects failed: %s", error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }

    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -EIO;
    }

    DBusMessageIter array_iter;
    dbus_message_iter_recurse(&iter, &array_iter);

    size_t matched = 0;

    while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict_entry;
        dbus_message_iter_recurse(&array_iter, &dict_entry);

        if (dbus_message_iter_get_arg_type(&dict_entry) != DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        dbus_message_iter_next(&dict_entry);

        if (dbus_message_iter_get_arg_type(&dict_entry) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        DBusMessageIter iface_iter;
        dbus_message_iter_recurse(&dict_entry, &iface_iter);

        struct mesh_bluez_device_info info;
        memset(&info, 0, sizeof(info));
        bool has_service = false;

        while (dbus_message_iter_get_arg_type(&iface_iter) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter iface_entry;
            dbus_message_iter_recurse(&iface_iter, &iface_entry);

            if (dbus_message_iter_get_arg_type(&iface_entry) != DBUS_TYPE_STRING) {
                dbus_message_iter_next(&iface_iter);
                continue;
            }

            const char *interface_name = NULL;
            dbus_message_iter_get_basic(&iface_entry, &interface_name);
            dbus_message_iter_next(&iface_entry);

            if (dbus_message_iter_get_arg_type(&iface_entry) != DBUS_TYPE_ARRAY) {
                dbus_message_iter_next(&iface_iter);
                continue;
            }

            if (interface_name == NULL || strcmp(interface_name, "org.bluez.Device1") != 0) {
                dbus_message_iter_next(&iface_iter);
                continue;
            }

            DBusMessageIter props_iter;
            dbus_message_iter_recurse(&iface_entry, &props_iter);

            while (dbus_message_iter_get_arg_type(&props_iter) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter prop_entry;
                dbus_message_iter_recurse(&props_iter, &prop_entry);

                if (dbus_message_iter_get_arg_type(&prop_entry) != DBUS_TYPE_STRING) {
                    dbus_message_iter_next(&props_iter);
                    continue;
                }

                const char *property_name = NULL;
                dbus_message_iter_get_basic(&prop_entry, &property_name);
                dbus_message_iter_next(&prop_entry);

                DBusMessageIter variant_iter;
                dbus_message_iter_recurse(&prop_entry, &variant_iter);

                int variant_type = dbus_message_iter_get_arg_type(&variant_iter);
                if (strcmp(property_name, "UUIDs") == 0 && variant_type == DBUS_TYPE_ARRAY) {
                    DBusMessageIter uuid_array;
                    dbus_message_iter_recurse(&variant_iter, &uuid_array);
                    while (dbus_message_iter_get_arg_type(&uuid_array) == DBUS_TYPE_STRING) {
                        const char *uuid = NULL;
                        dbus_message_iter_get_basic(&uuid_array, &uuid);
                        if (uuid_equals_meshtastic(uuid)) {
                            has_service = true;
                        }
                        dbus_message_iter_next(&uuid_array);
                    }
                } else if (strcmp(property_name, "Address") == 0 &&
                           variant_type == DBUS_TYPE_STRING) {
                    const char *address = NULL;
                    dbus_message_iter_get_basic(&variant_iter, &address);
                    if (address != NULL) {
                        snprintf(info.address, sizeof(info.address), "%s", address);
                    }
                } else if ((strcmp(property_name, "Name") == 0 ||
                            strcmp(property_name, "Alias") == 0) &&
                           variant_type == DBUS_TYPE_STRING && info.name[0] == '\0') {
                    const char *name = NULL;
                    dbus_message_iter_get_basic(&variant_iter, &name);
                    if (name != NULL) {
                        snprintf(info.name, sizeof(info.name), "%s", name);
                    }
                } else if (strcmp(property_name, "Paired") == 0 &&
                           variant_type == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t paired = FALSE;
                    dbus_message_iter_get_basic(&variant_iter, &paired);
                    info.paired = (paired != FALSE);
                } else if (strcmp(property_name, "RSSI") == 0 && variant_type == DBUS_TYPE_INT16) {
                    int16_t rssi = 0;
                    dbus_message_iter_get_basic(&variant_iter, &rssi);
                    info.rssi = rssi;
                }

                dbus_message_iter_next(&props_iter);
            }

            dbus_message_iter_next(&iface_iter);
        }

        if (has_service) {
            if (matched < capacity) {
                devices[matched] = info;
            } else {
                mesh_log_warn("ble", "Device cache full, dropping entry");
            }
            ++matched;
        }

        dbus_message_iter_next(&array_iter);
    }

    dbus_message_unref(reply);

    if (matched > capacity) {
        matched = capacity;
    }
    *count = matched;
    return 0;
#else
    (void)devices;
    (void)capacity;
    (void)count;
    return -ENOSYS;
#endif
}
