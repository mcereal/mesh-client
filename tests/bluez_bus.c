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
    (void)read(fd, &count, sizeof count);
    ++*(unsigned *)userdata;
    return 0;
}

static DBusMessage *request(DBusConnection *server, struct mesh_event_loop *loop) {
    for (unsigned turn = 0U; turn < 100U; ++turn) {
        mesh_event_loop_run(loop, 0);
        dbus_connection_read_write(server, 10);
        DBusMessage *message;
        while ((message = dbus_connection_pop_message(server)) != NULL) {
            if (dbus_message_is_method_call(message, "org.bluez.GattCharacteristic1",
                                            "ReadValue")) {
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
        call = request(server, &loop);
        if (call == NULL) {
            failure = "queued read never reached the fake service";
            break;
        }
        if (strcmp(dbus_message_get_signature(call), "a{sv}") != 0) {
            failure = "ReadValue options were not marshalled correctly";
            break;
        }
        const uint64_t one = 1U;
        (void)write(input_fd, &one, sizeof one);
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
