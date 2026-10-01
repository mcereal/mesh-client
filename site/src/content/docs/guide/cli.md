---
title: The command line
description: Use meshclient from a terminal to check a radio, send messages or flash firmware.
---

The same binary that runs on the Brick also works as a command-line tool on Linux and macOS.
The [Linux download](../install/#linux-command-line) is one static file.

`meshclient --help` is the full list. These are the commands you'll use most.

## Looking around

```bash
meshclient --list-devices                 # nearby Bluetooth radios and USB ports
meshclient --status                       # connect and print a summary
meshclient --status --json                # the same, as JSON for scripts
```

## Sending a message

```bash
meshclient --send-text "hello mesh"                          # to channel 0
meshclient --send-text "net in 5" --channel 2                # to another channel
meshclient --send-text "on my way" --dest '!433d1a2c' --ack  # direct, wait for the ack
```

`--dest` takes a node ID as `!hex`, `0xhex` or decimal. `--ack` only works with direct
messages, because the mesh never acknowledges a channel message.

## Choosing the link

Bluetooth is the default.

```bash
meshclient --status --serial                    # the first USB radio found
meshclient --status --serial=/dev/ttyUSB0       # a particular port
meshclient --status --tcp-host 192.168.1.50     # over the network, port 4403
```

On Linux, Bluetooth needs BlueZ running (`bluetoothd` and the system D-Bus).

## Running continuously

```bash
meshclient --foreground --log-level debug
```

Without `--foreground`, a command does one job and exits. With it, the client keeps running,
reconnects on its own and serves the radio, which is how the Brick runs it.

## Flashing firmware

```bash
meshclient --fetch-firmware heltec-v3 --staging /tmp            # download only
meshclient --install-firmware heltec-v3 --staging /tmp --serial # write it over USB
```

The board name is the firmware's build target, such as `heltec-v3` or
`heltec-mesh-node-t114`. If an install is interrupted, the radio waits in its bootloader, and
running the same command again finishes it.

## Settings from the environment

Most behaviour can also be set with `MESHCLIENT_*` environment variables. A few useful ones:

| Variable | Effect |
|---|---|
| `MESHCLIENT_TCP_HOST` | the network radio to connect to |
| `MESHCLIENT_AUTOCONNECT=0` | don't connect on your own |
| `MESHCLIENT_THEME` | `dark`, `light`, `contrast` or `colorblind` |
| `MESHCLIENT_LANG` | the language the UI is drawn in |
| `MESHCLIENT_PROTOCOL` | `meshtastic` or `meshcore`, to skip asking a USB or network radio |

The [full reference](https://github.com/mcereal/mesh-client/blob/main/docs/cli.md) lists them
all.
