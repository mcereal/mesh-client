#define _POSIX_C_SOURCE 200809L

/* Run only under dbus-run-session: the fake service and client use its isolated address,
   never the host's system bus or real BlueZ. */
#include "mesh/core/event_loop.h"
#include "mesh/transport/ble_bluez.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

static void ready(void *userdata) { ++*(unsigned *)userdata; }
static int input(int fd, uint32_t events, void *userdata) {
    (void)events;
    uint64_t count;
    if (read(fd, &count, sizeof count) != sizeof count)
        return -EIO;
    ++*(unsigned *)userdata;
    return 0;
}

static DBusMessage *request_named(DBusConnection *server, struct mesh_event_loop *loop,
                                  const char *member) {
    for (unsigned turn = 0U; turn < 100U; ++turn) {
        mesh_event_loop_run(loop, 0);
        dbus_connection_read_write(server, 10);
        DBusMessage *message;
        while ((message = dbus_connection_pop_message(server)) != NULL) {
            if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL &&
                strcmp(dbus_message_get_member(message), member) == 0) {
                return message;
            }
            dbus_message_unref(message);
        }
    }
    return NULL;
}

static void respond(DBusConnection *server, DBusMessage *call, bool malformed) {
    DBusMessage *reply = dbus_message_new_method_return(call);
    const uint8_t payload[] = {0x08, 0x01, 0x12, 0x00};
    const uint8_t *ptr = payload;
    const char *wrong = "wrong type";
    if (malformed) {
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &wrong, DBUS_TYPE_INVALID);
    } else {
        dbus_message_append_args(reply, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &ptr, (int)sizeof payload,
                                 DBUS_TYPE_INVALID);
    }
    dbus_connection_send(server, reply, NULL);
    dbus_connection_flush(server);
    dbus_message_unref(reply);
}

static int operation(struct mesh_bluez_client *client, unsigned op, bool *value) {
    const uint8_t data[] = {0x08, 0x01};
    if (op == 0U)
        return mesh_bluez_client_write(client, "/toradio", MESH_BLE_TORADIO_UUID, data,
                                       sizeof data);
    if (op == 1U)
        return mesh_bluez_client_services_resolved(client, "/device", value);
    return mesh_bluez_client_device_connected(client, "/device", value);
}

static void operation_reply(DBusConnection *server, DBusMessage *call, unsigned op, unsigned pass) {
    DBusMessage *reply = pass == 1U
                             ? dbus_message_new_error(call, "org.bluez.Error.Failed", "failed")
                             : dbus_message_new_method_return(call);
    if (pass == 2U) {
        const char *wrong = "wrong";
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &wrong, DBUS_TYPE_INVALID);
    } else if (op != 0U && pass != 1U) {
        DBusMessageIter iter, variant;
        dbus_bool_t value = TRUE;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
        dbus_message_iter_close_container(&iter, &variant);
    }
    dbus_connection_send(server, reply, NULL);
    dbus_connection_flush(server);
    dbus_message_unref(reply);
}

static const char *test_operations(DBusConnection *server, struct mesh_event_loop *loop,
                                   struct mesh_bluez_client *client, int input_fd,
                                   unsigned *inputs) {
    for (unsigned op = 0U; op < 3U; ++op) {
        for (unsigned pass = 0U; pass < 5U; ++pass) {
            bool value = false;
            if (operation(client, op, &value) != -EAGAIN)
                return "operation did not yield";
            DBusMessage *call = request_named(server, loop, op == 0U ? "WriteValue" : "Get");
            if (call == NULL)
                return "operation never reached fake service";
            const bool signature =
                strcmp(dbus_message_get_signature(call), op == 0U ? "aya{sv}" : "ss") == 0;
            const unsigned before = *inputs;
            const uint64_t one = 1U;
            if (write(input_fd, &one, sizeof one) != sizeof one) {
                dbus_message_unref(call);
                return "input wake failed";
            }
            mesh_event_loop_run(loop, 0);
            if (!signature || *inputs != before + 1U || operation(client, op, &value) != -EAGAIN) {
                dbus_message_unref(call);
                return "pending operation blocked input, duplicated send or marshalled incorrectly";
            }
            if (pass >= 3U) {
                if (pass == 3U) {
                    const struct itimerspec spec = {.it_value = {.tv_nsec = 1L}};
                    timerfd_settime(client->requests[op].timer_fd, 0, &spec, NULL);
                    struct pollfd fd = {.fd = client->requests[op].timer_fd, .events = POLLIN};
                    (void)poll(&fd, 1, 1000);
                    mesh_event_loop_run(loop, 0);
                    if (operation(client, op, &value) != -ETIMEDOUT) {
                        dbus_message_unref(call);
                        return "operation timeout lost";
                    }
                } else {
                    mesh_bluez_client_requests_cancel(client);
                }
                /* A reply for the old link/request cannot finish the new request. */
                if (operation(client, op, &value) != -EAGAIN) {
                    dbus_message_unref(call);
                    return "operation did not restart";
                }
                DBusMessage *next = request_named(server, loop, op == 0U ? "WriteValue" : "Get");
                operation_reply(server, call, op, 0U);
                dbus_message_unref(call);
                call = next;
                mesh_event_loop_run(loop, 1);
                if (call == NULL || operation(client, op, &value) != -EAGAIN) {
                    if (call != NULL)
                        dbus_message_unref(call);
                    return "late reply completed a newer operation";
                }
            }
            operation_reply(server, call, op, pass < 3U ? pass : 0U);
            dbus_message_unref(call);
            for (unsigned turn = 0U; turn < 100U && client->requests[op].state == 1; ++turn) {
                mesh_event_loop_run(loop, 1);
            }
            const int expected = pass == 1U ? -EIO : pass == 2U ? -EPROTO : 0;
            if (operation(client, op, &value) != expected ||
                (expected == 0 && op != 0U && !value)) {
                return "operation reply result incorrect";
            }
        }
    }
    return NULL;
}

int main(void) {
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (address == NULL) {
        fputs("This test requires dbus-run-session.\n", stderr);
        return 1;
    }
    DBusError error;
    dbus_error_init(&error);
    DBusConnection *server = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (server == NULL || dbus_bus_request_name(server, "org.bluez", 0, &error) !=
                              DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fputs("Could not start isolated fake service.\n", stderr);
        return 1;
    }
    struct mesh_bluez_mock_config mock = {.read_bus_address = address};
    mesh_bluez_client_mock_enable(&mock);
    struct mesh_bluez_client client = {0};
    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0 || mesh_bluez_client_init(&client) != 0 ||
        mesh_bluez_client_attach_loop(&client, &loop) != 0) {
        fputs("Could not initialize isolated client.\n", stderr);
        return 1;
    }
    unsigned completions = 0U;
    unsigned inputs = 0U;
    client.read_ready = ready;
    client.read_userdata = &completions;
    int input_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    mesh_event_loop_add_fd(&loop, input_fd, EPOLLIN, input, &inputs);
    const char *failure = NULL;
    uint8_t bytes[512];
    size_t length;
    DBusMessage *call = NULL;
    for (unsigned pass = 0U; pass < 4U; ++pass) {
        if (mesh_bluez_client_read(&client, "/fromradio", bytes, sizeof bytes, &length) !=
                -EAGAIN ||
            length != 0U) {
            failure = "read did not yield";
            break;
        }
        call = request_named(server, &loop, "ReadValue");
        if (call == NULL) {
            failure = "queued read never reached the fake service";
            break;
        }
        if (strcmp(dbus_message_get_signature(call), "a{sv}") != 0) {
            failure = "ReadValue options were not marshalled correctly";
            break;
        }
        const uint64_t one = 1U;
        if (write(input_fd, &one, sizeof one) != sizeof one) {
            failure = "input wake failed";
            break;
        }
        mesh_event_loop_run(&loop, 0);
        if (inputs != pass + 1U || completions != pass) {
            failure = "input must be serviced while the reply is withheld";
            break;
        }
        if (pass == 2U) {
            const struct itimerspec spec = {.it_value = {.tv_nsec = 1L}};
            timerfd_settime(client.read_timer_fd, 0, &spec, NULL);
            struct pollfd fd = {.fd = client.read_timer_fd, .events = POLLIN};
            (void)poll(&fd, 1, 1000);
        } else {
            respond(server, call, pass == 1U);
        }
        for (unsigned turn = 0U; turn < 100U && completions == pass; ++turn) {
            mesh_event_loop_run(&loop, 1);
        }
        const int result =
            mesh_bluez_client_read(&client, "/fromradio", bytes, sizeof bytes, &length);
        const int expected = pass == 1U ? -EPROTO : pass == 2U ? -ETIMEDOUT : 0;
        if (completions != pass + 1U || result != expected ||
            (result == 0 && (length != 4U || bytes[0] != 0x08U))) {
            failure = "reply, malformed payload or timeout completion was incorrect";
            break;
        }
        if (pass == 2U) {
            /* A timed-out reply must not complete the following read. */
            respond(server, call, false);
            mesh_event_loop_run(&loop, 1);
            if (completions != pass + 1U) {
                failure = "late reply was not discarded";
                break;
            }
        }
        dbus_message_unref(call);
        call = NULL;
    }
    if (call != NULL) {
        dbus_message_unref(call);
    }
    if (failure == NULL) {
        failure = test_operations(server, &loop, &client, input_fd, &inputs);
    }
    mesh_event_loop_remove_fd(&loop, input_fd);
    close(input_fd);
    mesh_bluez_client_shutdown(&client);
    mesh_event_loop_shutdown(&loop);
    mesh_bluez_client_mock_disable();
    dbus_connection_close(server);
    dbus_connection_unref(server);
    dbus_error_free(&error);
    if (failure != NULL) {
        fprintf(stderr, "%s\n", failure);
        return 1;
    }
    puts("Isolated D-Bus: nonblocking send, input responsiveness, reply parsing and timeout "
         "passed.");
    return 0;
}
