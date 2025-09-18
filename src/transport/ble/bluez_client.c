#include "mesh/transport/ble_bluez.h"

#include "mesh/log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MESHTASTIC_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"

#ifndef DBUS_TIMEOUT_USE_DEFAULT
#define DBUS_TIMEOUT_USE_DEFAULT -1
#endif

#ifdef MESH_HAVE_DBUS
#include <dbus/dbus.h>
#endif

struct mesh_bluez_mock_state {
    bool enabled;
    struct mesh_bluez_mock_config config;
};

static struct mesh_bluez_mock_state g_mock_state;

static void mesh_bluez_apply_mock_devices(struct mesh_bluez_device_info *devices, size_t capacity, size_t *count) {
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
}

void mesh_bluez_client_mock_disable(void) {
    g_mock_state.enabled = false;
    memset(&g_mock_state.config, 0, sizeof(g_mock_state.config));
}

int mesh_bluez_client_init(struct mesh_bluez_client *client) {
    if (client == NULL) {
        return -EINVAL;
    }

    client->connection = NULL;
    client->connected = false;

#ifdef MESH_HAVE_DBUS
    if (g_mock_state.enabled) {
        if (g_mock_state.config.init_result < 0) {
            return g_mock_state.config.init_result;
        }
        client->connected = true;
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

    client->connection = connection;
    client->connected = true;
    return 0;
#else
    if (g_mock_state.enabled) {
        if (g_mock_state.config.init_result < 0) {
            return g_mock_state.config.init_result;
        }
        client->connected = true;
        return 0;
    }
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

static int call_adapter_method(struct mesh_bluez_client *client, const char *adapter_path, const char *method) {
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

int mesh_bluez_client_connect(struct mesh_bluez_client *client, const char *device_path) {
    if (client == NULL || device_path == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        return g_mock_state.config.connect_result;
    }

#ifdef MESH_HAVE_DBUS
    (void)client;
    (void)device_path;
    return -ENOSYS;
#else
    return -ENOSYS;
#endif
}

int mesh_bluez_client_disconnect(struct mesh_bluez_client *client, const char *device_path) {
    if (client == NULL || device_path == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        return g_mock_state.config.disconnect_result;
    }

#ifdef MESH_HAVE_DBUS
    (void)client;
    (void)device_path;
    return -ENOSYS;
#else
    return -ENOSYS;
#endif
}

int mesh_bluez_client_subscribe(struct mesh_bluez_client *client, const char *device_path, const char *char_uuid) {
    if (client == NULL || device_path == NULL || char_uuid == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        return g_mock_state.config.subscribe_result;
    }

#ifdef MESH_HAVE_DBUS
    (void)client;
    (void)device_path;
    (void)char_uuid;
    return -ENOSYS;
#else
    return -ENOSYS;
#endif
}

int mesh_bluez_client_write(struct mesh_bluez_client *client, const char *device_path, const char *char_uuid,
                            const uint8_t *data, size_t len) {
    if (client == NULL || device_path == NULL || char_uuid == NULL || data == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        return g_mock_state.config.write_result;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    DBusMessage *message = dbus_message_new_method_call("org.bluez", device_path,
                                                       "org.bluez.GattCharacteristic1", "WriteValue");
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
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(connection, message, DBUS_TIMEOUT_USE_DEFAULT,
                                                                   &error);
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
    return -ENOSYS;
#endif
}

#ifdef MESH_HAVE_DBUS
static int mesh_bluez_find_characteristics(DBusConnection *connection, const char *device_path, const char *uuid,
                                           char *out_path, size_t out_len) {
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

            if (interface_name == NULL || strcmp(interface_name, "org.bluez.GattCharacteristic1") != 0) {
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

int mesh_bluez_client_find_nus_characteristics(struct mesh_bluez_client *client, const char *device_path,
                                               char *rx_path, size_t rx_len, char *tx_path, size_t tx_len) {
    if (client == NULL || device_path == NULL || rx_path == NULL || tx_path == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        if (g_mock_state.config.rx_char_path != NULL) {
            snprintf(rx_path, rx_len, "%s", g_mock_state.config.rx_char_path);
        } else {
            snprintf(rx_path, rx_len, "%s/nus_rx", device_path);
        }
        if (g_mock_state.config.tx_char_path != NULL) {
            snprintf(tx_path, tx_len, "%s", g_mock_state.config.tx_char_path);
        } else {
            snprintf(tx_path, tx_len, "%s/nus_tx", device_path);
        }
        return 0;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection *connection = (DBusConnection *)client->connection;
    int rx_result = mesh_bluez_find_characteristics(connection, device_path, MESH_BLE_NUS_RX_UUID, rx_path, rx_len);
    if (rx_result < 0) {
        return rx_result;
    }
    int tx_result = mesh_bluez_find_characteristics(connection, device_path, MESH_BLE_NUS_TX_UUID, tx_path, tx_len);
    if (tx_result < 0) {
        return tx_result;
    }
    return 0;
#else
    (void)client;
    (void)device_path;
    (void)rx_path;
    (void)rx_len;
    (void)tx_path;
    (void)tx_len;
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
    return strcmp(buffer, MESHTASTIC_SERVICE_UUID) == 0;
}
#endif

int mesh_bluez_client_list_meshtastic(struct mesh_bluez_client *client, struct mesh_bluez_device_info *devices,
                                      size_t capacity, size_t *count) {
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
        return strcmp(buffer, MESHTASTIC_SERVICE_UUID) == 0;
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
                } else if (strcmp(property_name, "Address") == 0 && variant_type == DBUS_TYPE_STRING) {
                    const char *address = NULL;
                    dbus_message_iter_get_basic(&variant_iter, &address);
                    if (address != NULL) {
                        snprintf(info.address, sizeof(info.address), "%s", address);
                    }
                } else if ((strcmp(property_name, "Name") == 0 || strcmp(property_name, "Alias") == 0) &&
                           variant_type == DBUS_TYPE_STRING && info.name[0] == '\0') {
                    const char *name = NULL;
                    dbus_message_iter_get_basic(&variant_iter, &name);
                    if (name != NULL) {
                        snprintf(info.name, sizeof(info.name), "%s", name);
                    }
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
