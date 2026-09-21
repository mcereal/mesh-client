#pragma once

/*
 * The control socket: the running client, driven by name from outside it.
 *
 * A developer - or an agent - pressing keys in a window cannot see what a press did without a
 * screenshot of the whole desktop, and cannot press anything at all on a Brick across the room
 * or in a cloud session with no display. This is a Unix socket the client listens on when
 * `--ui-control PATH` (or MESHCLIENT_UI_CONTROL) names one, taking one command a line and
 * answering each with one line:
 *
 *   ping              ok
 *   key NAME...       each key pressed in turn, through the path a button takes - see
 *                     inkcell_key_from_name() for the names ("a", "l1", "select", "down")
 *   wait MS           nothing, for MS milliseconds
 *   shot PATH         the frame, as a PPM at PATH, once the panel has stopped moving
 *   screen            the tab that is up, by the id a scene file names it with
 *   quit              the client stops, as a quit key would stop it
 *
 * An answer is `ok`, `ok DETAIL`, or `error REASON`. A command waits for the one before it: a
 * `shot` after a `key` is a picture of what the key did.
 *
 * Off unless asked for, and 0600 when on: the socket presses keys on a radio client, and
 * nothing that did not start the client has any business doing that. One connection at a time;
 * a second is told `error busy`.
 *
 * `shot` needs a backend that draws pixels - fb, sdl or headless. The terminal backend has no
 * frame to give.
 */

#include "inkwell/runtime/loop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_controller;

/* sun_path is 104 bytes on a Mac and 108 on Linux; the smaller of the two, with its NUL. */
#define MESH_APP_CONTROL_PATH_MAX 104U
#define MESH_APP_CONTROL_LINE_MAX 1024U

/* How long a `shot` waits for the panel to settle before taking whatever is up. A screen that
   never settles - a map still filling tiles - is still worth a picture. */
#define MESH_APP_CONTROL_SETTLE_MS 2000U
/* How often it looks. */
#define MESH_APP_CONTROL_POLL_MS 40U

enum mesh_app_control_wait {
    MESH_APP_CONTROL_READY = 0,
    MESH_APP_CONTROL_SLEEPING,
    MESH_APP_CONTROL_SETTLING,
};

struct mesh_app_control {
    struct inkwell_loop *loop;
    struct mesh_ui_controller *controller;
    char path[MESH_APP_CONTROL_PATH_MAX];
    int listen_fd;
    int client_fd;
    int timer_fd;
    char in[MESH_APP_CONTROL_LINE_MAX];
    size_t in_len;
    enum mesh_app_control_wait waiting;
    uint64_t deadline_ms;
    char shot_path[MESH_APP_CONTROL_LINE_MAX];
};

/*
 * Listens on `path`. Anything already there is refused (-EADDRINUSE) rather than removed, a
 * socket a killed run left behind included - see the note in app_control.c. Returns 0 or a
 * negative errno; the client runs without the socket either way.
 */
int mesh_app_control_open(struct mesh_app_control *control, struct inkwell_loop *loop,
                          struct mesh_ui_controller *controller, const char *path);
/* Closes the connection and the socket, and removes the socket's file. Safe on a control that
   was never opened, provided it was zeroed. */
void mesh_app_control_close(struct mesh_app_control *control);

/*
 * The other end: connects to `path`, sends `commands` - separated by newlines or by `;` - one at
 * a time, and prints each answer to `out`. Stops at the first `error`.
 *
 * What `meshclient --ui-send` runs, so that a Brick, which has no `nc` that speaks Unix sockets,
 * is driven by the binary it already has. Returns 0 when every command answered `ok`, -EPROTO
 * when one answered `error`, or a negative errno when the socket did not answer at all.
 */
int mesh_app_control_send(const char *path, const char *commands, FILE *out);

#ifdef __cplusplus
}
#endif
