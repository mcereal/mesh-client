#include "mesh/transport/ble_bluez.h"

#include "mesh/log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef DBUS_TIMEOUT_USE_DEFAULT
#define DBUS_TIMEOUT_USE_DEFAULT -1
#endif

#ifdef MESH_HAVE_DBUS
#include <dbus/dbus.h>
#endif

int mesh_bluez_client_init(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return -EINVAL;
    }

    client->connection = NULL;
    client->connected = false;

#ifdef MESH_HAVE_DBUS
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

    client->connection = connection;
    client->connected = true;
    return 0;
#else
    mesh_log_debug("bluez", "DBus support disabled at build time");
    return -ENOSYS;
#endif
}

void mesh_bluez_client_shutdown(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return;
    }

#ifdef MESH_HAVE_DBUS
    if (client->connected && client->connection != NULL) {
        DBusConnection *connection = (DBusConnection *)client->connection;
        dbus_connection_unref(connection);
    }
#endif

    client->connection = NULL;
    client->connected = false;
}

int mesh_bluez_client_check_ready(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return -EINVAL;
    }

#ifdef MESH_HAVE_DBUS
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
    (void)client;
    return -ENOSYS;
#endif
}

int mesh_bluez_client_find_adapter(struct mesh_bluez_client *client, char *path, size_t path_len) {
    if (client == NULL || path == NULL || path_len == 0U) {
        return -EINVAL;
    }

#ifdef MESH_HAVE_DBUS
    if (!client->connected || client->connection == NULL) {
        return -ENOTCONN;
    }

    DBusConnection *connection = (DBusConnection *)client->connection;

    DBusMessage *message = dbus_message_new_method_call("org.bluez", "/",
                                                       "org.freedesktop.DBus.ObjectManager",
                                                       "GetManagedObjects");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(connection, message, 1000, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "GetManagedObjects failed: %s", error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }

    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
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
    (void)client;
    (void)path;
    (void)path_len;
    return -ENOSYS;
#endif
}

static int call_adapter_method(struct mesh_bluez_client *client, const char *adapter_path, const char *method) {
#ifdef MESH_HAVE_DBUS
    if (client == NULL || adapter_path == NULL || method == NULL) {
        return -EINVAL;
    }

    if (!client->connected || client->connection == NULL) {
        return -ENOTCONN;
    }

    DBusConnection *connection = (DBusConnection *)client->connection;
    DBusMessage *message = dbus_message_new_method_call("org.bluez", adapter_path, "org.bluez.Adapter1", method);
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(connection, message, DBUS_TIMEOUT_USE_DEFAULT,
                                                                   &error);
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
