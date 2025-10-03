#include "mesh/transport/ble_bluez.h"

#include "mesh/event_loop.h"
#include "mesh/log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>

#define MESHTASTIC_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"

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

static int mesh_bluez_watch_fd_callback(int fd, uint32_t events, void* userdata);

static int mesh_bluez_watch_sync(struct mesh_bluez_client* client, size_t index) {
    if (client == NULL || index >= sizeof(client->watches) / sizeof(client->watches[0])) {
        return -EINVAL;
    }

    struct mesh_event_loop* loop = client->loop;
    if (loop == NULL) {
        return 0;
    }

    struct mesh_bluez_watch_entry* entry = &client->watches[index];
    if (entry->watch == NULL) {
        return 0;
    }

    entry->fd     = dbus_watch_get_unix_fd(entry->watch);
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

static void mesh_bluez_watch_unregister(struct mesh_bluez_client* client, size_t index) {
    if (client == NULL || client->loop == NULL ||
        index >= sizeof(client->watches) / sizeof(client->watches[0])) {
        return;
    }

    struct mesh_bluez_watch_entry* entry = &client->watches[index];
    if (!entry->registered) {
        return;
    }

    mesh_event_loop_remove_fd(client->loop, entry->fd);
    entry->registered = false;
}

static ssize_t mesh_bluez_watch_find(struct mesh_bluez_client* client, DBusWatch* watch) {
    if (client == NULL) {
        return -1;
    }

    for (size_t i = 0; i < sizeof(client->watches) / sizeof(client->watches[0]); ++i) {
        if (client->watches[i].watch == watch) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static dbus_bool_t mesh_bluez_watch_add(DBusWatch* watch, void* userdata) {
    struct mesh_bluez_client* client = (struct mesh_bluez_client*)userdata;
    if (client == NULL || watch == NULL) {
        return FALSE;
    }

    for (size_t i = 0; i < sizeof(client->watches) / sizeof(client->watches[0]); ++i) {
        if (client->watches[i].watch == NULL) {
            client->watches[i].watch      = watch;
            client->watches[i].registered = false;
            client->watches[i].client     = client;
            mesh_bluez_watch_sync(client, i);
            return TRUE;
        }
    }

    mesh_log_warn("bluez", "No space for additional D-Bus watches");
    return FALSE;
}

static void mesh_bluez_watch_remove(DBusWatch* watch, void* userdata) {
    struct mesh_bluez_client* client = (struct mesh_bluez_client*)userdata;
    if (client == NULL || watch == NULL) {
        return;
    }

    ssize_t index = mesh_bluez_watch_find(client, watch);
    if (index < 0) {
        return;
    }

    mesh_bluez_watch_unregister(client, (size_t)index);
    struct mesh_bluez_watch_entry* entry = &client->watches[index];
    entry->watch                         = NULL;
    entry->fd                            = -1;
    entry->events                        = 0U;
    entry->registered                    = false;
    entry->client                        = NULL;
}

static void mesh_bluez_watch_toggled(DBusWatch* watch, void* userdata) {
    struct mesh_bluez_client* client = (struct mesh_bluez_client*)userdata;
    if (client == NULL || watch == NULL) {
        return;
    }

    ssize_t index = mesh_bluez_watch_find(client, watch);
    if (index < 0) {
        return;
    }

    mesh_bluez_watch_sync(client, (size_t)index);
}

static int mesh_bluez_watch_fd_callback(int fd, uint32_t events, void* userdata) {
    (void)fd;
    struct mesh_bluez_watch_entry* entry = (struct mesh_bluez_watch_entry*)userdata;
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
    struct mesh_bluez_client* client;
};

static struct mesh_bluez_mock_state g_mock_state;

static void mesh_bluez_apply_mock_devices(struct mesh_bluez_device_info* devices, size_t capacity,
                                          size_t* count) {
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

void mesh_bluez_client_mock_enable(const struct mesh_bluez_mock_config* config) {
    g_mock_state.enabled = true;
    if (config != NULL) {
        g_mock_state.config = *config;
    } else {
        memset(&g_mock_state.config, 0, sizeof(g_mock_state.config));
    }
    g_mock_state.client = NULL;
}

void mesh_bluez_client_mock_disable(void) {
    g_mock_state.enabled = false;
    memset(&g_mock_state.config, 0, sizeof(g_mock_state.config));
    g_mock_state.client = NULL;
}

int mesh_bluez_client_init(struct mesh_bluez_client* client) {
    if (client == NULL) {
        return -EINVAL;
    }

    client->connection                    = NULL;
    client->connected                     = false;
    client->loop                          = NULL;
    client->notification_callback         = NULL;
    client->notification_userdata         = NULL;
    client->notify_characteristic_path[0] = '\0';
#ifdef MESH_HAVE_DBUS
    memset(client->watches, 0, sizeof(client->watches));
#endif

#ifdef MESH_HAVE_DBUS
    if (g_mock_state.enabled) {
        if (g_mock_state.config.init_result < 0) {
            return g_mock_state.config.init_result;
        }
        client->connected   = true;
        g_mock_state.client = client;
        return 0;
    }

    DBusError error;
    dbus_error_init(&error);

    DBusConnection* connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
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

    client->connection  = connection;
    client->connected   = true;
    g_mock_state.client = NULL;
    return 0;
#else
    if (g_mock_state.enabled) {
        if (g_mock_state.config.init_result < 0) {
            return g_mock_state.config.init_result;
        }
        client->connected   = true;
        g_mock_state.client = client;
        return 0;
    }
    mesh_log_debug("bluez", "DBus support disabled at build time");
    return -ENOSYS;
#endif
}

void mesh_bluez_client_mock_emit_notification(const char* char_path, const uint8_t* data,
                                              size_t len) {
    if (!g_mock_state.enabled) {
        return;
    }

    struct mesh_bluez_client* client = g_mock_state.client;
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

void mesh_bluez_client_shutdown(struct mesh_bluez_client* client) {
    if (client == NULL) {
        return;
    }

#ifdef MESH_HAVE_DBUS
    if (client->loop != NULL) {
        mesh_bluez_client_detach_loop(client);
    }
#endif

#ifdef MESH_HAVE_DBUS
    if (client->connected && client->connection != NULL) {
        DBusConnection* connection = (DBusConnection*)client->connection;
        dbus_connection_set_watch_functions(connection, NULL, NULL, NULL, NULL, NULL);
        dbus_connection_unref(connection);
    }
#endif

    client->connection                    = NULL;
    client->connected                     = false;
    client->loop                          = NULL;
    client->notification_callback         = NULL;
    client->notification_userdata         = NULL;
    client->notify_characteristic_path[0] = '\0';
#ifdef MESH_HAVE_DBUS
    memset(client->watches, 0, sizeof(client->watches));
#endif
    if (g_mock_state.client == client) {
        g_mock_state.client = NULL;
    }
}

int mesh_bluez_client_check_ready(struct mesh_bluez_client* client) {
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

    DBusConnection* connection = (DBusConnection*)client->connection;

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
static void mesh_bluez_client_handle_properties_changed(struct mesh_bluez_client* client,
                                                        DBusMessage* message) {
    if (client == NULL || message == NULL) {
        return;
    }

    if (client->notify_characteristic_path[0] == '\0') {
        return;
    }

    const char* path = dbus_message_get_path(message);
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

    const char* interface_name = NULL;
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

        const char* property_name = NULL;
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
        if (dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        const uint8_t* payload = NULL;
        int length             = 0;
        dbus_message_iter_get_fixed_array(&variant_iter, &payload, &length);
        if (payload != NULL && length > 0 && client->notification_callback != NULL) {
            client->notification_callback(payload, (size_t)length, client->notification_userdata);
        }
        break;
    }
}

static void mesh_bluez_client_handle_message(struct mesh_bluez_client* client,
                                             DBusMessage* message) {
    if (client == NULL || message == NULL) {
        return;
    }

    if (dbus_message_is_signal(message, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
        mesh_bluez_client_handle_properties_changed(client, message);
    }
}

static int mesh_bluez_client_add_properties_match(struct mesh_bluez_client* client,
                                                  const char* path) {
    if (client == NULL || path == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        return 0;
    }

    DBusConnection* connection = (DBusConnection*)client->connection;
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

static void mesh_bluez_client_remove_properties_match(struct mesh_bluez_client* client,
                                                      const char* path) {
    if (client == NULL || path == NULL || path[0] == '\0') {
        return;
    }

    if (g_mock_state.enabled) {
        return;
    }

    DBusConnection* connection = (DBusConnection*)client->connection;
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

int mesh_bluez_client_attach_loop(struct mesh_bluez_client* client, struct mesh_event_loop* loop) {
    if (client == NULL) {
        return -EINVAL;
    }

    client->loop = loop;

#ifdef MESH_HAVE_DBUS
    if (loop != NULL) {
        for (size_t i = 0; i < sizeof(client->watches) / sizeof(client->watches[0]); ++i) {
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

void mesh_bluez_client_detach_loop(struct mesh_bluez_client* client) {
    if (client == NULL) {
        return;
    }

#ifdef MESH_HAVE_DBUS
    if (client->loop != NULL) {
        for (size_t i = 0; i < sizeof(client->watches) / sizeof(client->watches[0]); ++i) {
            if (client->watches[i].registered) {
                mesh_event_loop_remove_fd(client->loop, client->watches[i].fd);
            }
            client->watches[i].registered = false;
        }
    }
#endif

    client->loop = NULL;
}

int mesh_bluez_client_process(struct mesh_bluez_client* client) {
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

    DBusConnection* connection = (DBusConnection*)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    dbus_connection_read_write(connection, 0);

    DBusMessage* message = NULL;
    while ((message = dbus_connection_pop_message(connection)) != NULL) {
        mesh_bluez_client_handle_message(client, message);
        dbus_message_unref(message);
    }

    return 0;
#else
    return 0;
#endif
}

void mesh_bluez_client_set_notification_handler(struct mesh_bluez_client* client,
                                                mesh_bluez_notification_callback callback,
                                                void* userdata) {
    if (client == NULL) {
        return;
    }

    client->notification_callback = callback;
    client->notification_userdata = userdata;
}

int mesh_bluez_client_find_adapter(struct mesh_bluez_client* client, char* path, size_t path_len) {
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

    DBusConnection* connection = (DBusConnection*)client->connection;

    DBusMessage* message = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply =
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

    bool found                 = false;
    DBusMessageIter array_iter = iter;
    dbus_message_iter_recurse(&iter, &array_iter);

    while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter dict_entry;
        dbus_message_iter_recurse(&array_iter, &dict_entry);

        if (dbus_message_iter_get_arg_type(&dict_entry) != DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_next(&array_iter);
            continue;
        }

        const char* object_path = NULL;
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

            const char* interface_name = NULL;
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

static int call_adapter_method(struct mesh_bluez_client* client, const char* adapter_path,
                               const char* method) {
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

    DBusConnection* connection = (DBusConnection*)client->connection;
    DBusMessage* message =
        dbus_message_new_method_call("org.bluez", adapter_path, "org.bluez.Adapter1", method);
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
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

int mesh_bluez_client_start_discovery(struct mesh_bluez_client* client, const char* adapter_path) {
    return call_adapter_method(client, adapter_path, "StartDiscovery");
}

int mesh_bluez_client_stop_discovery(struct mesh_bluez_client* client, const char* adapter_path) {
    return call_adapter_method(client, adapter_path, "StopDiscovery");
}

int mesh_bluez_client_connect(struct mesh_bluez_client* client, const char* device_path) {
    if (client == NULL || device_path == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        if (g_mock_state.config.connect_result == 0) {
            g_mock_state.client = client;
        }
        return g_mock_state.config.connect_result;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection* connection = (DBusConnection*)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage* message =
        dbus_message_new_method_call("org.bluez", device_path, "org.bluez.Device1", "Connect");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        connection, message, DBUS_TIMEOUT_USE_DEFAULT, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "Connect failed: %s", error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }

    dbus_message_unref(reply);
    return 0;
#else
    (void)client;
    (void)device_path;
    return -ENOSYS;
#endif
}

int mesh_bluez_client_disconnect(struct mesh_bluez_client* client, const char* device_path) {
    if (client == NULL || device_path == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        int result = g_mock_state.config.disconnect_result;
        if (result == 0 && g_mock_state.client == client) {
            g_mock_state.client                   = NULL;
            client->notify_characteristic_path[0] = '\0';
        }
        return result;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection* connection = (DBusConnection*)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage* message =
        dbus_message_new_method_call("org.bluez", device_path, "org.bluez.Device1", "Disconnect");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
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

int mesh_bluez_client_subscribe(struct mesh_bluez_client* client, const char* device_path,
                                const char* char_uuid) {
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
    DBusConnection* connection = (DBusConnection*)client->connection;
    if (connection == NULL) {
        return -ENOTCONN;
    }

    DBusMessage* message = dbus_message_new_method_call(
        "org.bluez", device_path, "org.bluez.GattCharacteristic1", "StartNotify");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        connection, message, DBUS_TIMEOUT_USE_DEFAULT, &error);
    dbus_message_unref(message);

    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            mesh_log_warn("bluez", "StartNotify failed: %s", error.message);
            dbus_error_free(&error);
        }
        return -EIO;
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

int mesh_bluez_client_write(struct mesh_bluez_client* client, const char* device_path,
                            const char* char_uuid, const uint8_t* data, size_t len) {
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

        return g_mock_state.config.write_result;
    }

#ifdef MESH_HAVE_DBUS
    DBusConnection* connection = (DBusConnection*)client->connection;
    DBusMessage* message       = dbus_message_new_method_call(
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
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
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
static int mesh_bluez_find_characteristics(DBusConnection* connection, const char* device_path,
                                           const char* uuid, char* out_path, size_t out_len) {
    DBusMessage* message = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply =
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

        const char* object_path = NULL;
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

            const char* interface_name = NULL;
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

                const char* property_name = NULL;
                dbus_message_iter_get_basic(&prop_entry, &property_name);
                dbus_message_iter_next(&prop_entry);

                if (strcmp(property_name, "UUID") == 0) {
                    DBusMessageIter variant_iter;
                    dbus_message_iter_recurse(&prop_entry, &variant_iter);
                    if (dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_STRING) {
                        const char* value = NULL;
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

int mesh_bluez_client_find_nus_characteristics(struct mesh_bluez_client* client,
                                               const char* device_path, char* rx_path,
                                               size_t rx_len, char* tx_path, size_t tx_len) {
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
    DBusConnection* connection = (DBusConnection*)client->connection;
    int rx_result = mesh_bluez_find_characteristics(connection, device_path, MESH_BLE_NUS_RX_UUID,
                                                    rx_path, rx_len);
    if (rx_result < 0) {
        return rx_result;
    }
    int tx_result = mesh_bluez_find_characteristics(connection, device_path, MESH_BLE_NUS_TX_UUID,
                                                    tx_path, tx_len);
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
static bool uuid_equals_meshtastic(const char* uuid) {
    if (uuid == NULL) {
        return false;
    }
    char buffer[37];
    size_t index = 0;
    for (const char* c = uuid; *c != '\0' && index < sizeof(buffer) - 1; ++c) {
        buffer[index++] = (char)toupper((unsigned char)*c);
    }
    buffer[index] = '\0';
    return strcmp(buffer, MESHTASTIC_SERVICE_UUID) == 0;
}
#endif

int mesh_bluez_client_list_meshtastic(struct mesh_bluez_client* client,
                                      struct mesh_bluez_device_info* devices, size_t capacity,
                                      size_t* count) {
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
    DBusConnection* connection = (DBusConnection*)client->connection;
    DBusMessage* message       = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (message == NULL) {
        return -ENOMEM;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply =
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

            const char* interface_name = NULL;
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

                const char* property_name = NULL;
                dbus_message_iter_get_basic(&prop_entry, &property_name);
                dbus_message_iter_next(&prop_entry);

                DBusMessageIter variant_iter;
                dbus_message_iter_recurse(&prop_entry, &variant_iter);

                int variant_type = dbus_message_iter_get_arg_type(&variant_iter);
                if (strcmp(property_name, "UUIDs") == 0 && variant_type == DBUS_TYPE_ARRAY) {
                    DBusMessageIter uuid_array;
                    dbus_message_iter_recurse(&variant_iter, &uuid_array);
                    while (dbus_message_iter_get_arg_type(&uuid_array) == DBUS_TYPE_STRING) {
                        const char* uuid = NULL;
                        dbus_message_iter_get_basic(&uuid_array, &uuid);
                        if (uuid_equals_meshtastic(uuid)) {
                            has_service = true;
                        }
                        dbus_message_iter_next(&uuid_array);
                    }
                } else if (strcmp(property_name, "Address") == 0 &&
                           variant_type == DBUS_TYPE_STRING) {
                    const char* address = NULL;
                    dbus_message_iter_get_basic(&variant_iter, &address);
                    if (address != NULL) {
                        snprintf(info.address, sizeof(info.address), "%s", address);
                    }
                } else if ((strcmp(property_name, "Name") == 0 ||
                            strcmp(property_name, "Alias") == 0) &&
                           variant_type == DBUS_TYPE_STRING && info.name[0] == '\0') {
                    const char* name = NULL;
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
