#include "mesh/transport/ble_bluez.h"

#include "mesh/log.h"

#include <errno.h>
#include <stdio.h>

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
