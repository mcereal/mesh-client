#!/bin/sh
# Set console device for applications requiring a TTY
if [ -e /dev/tty0 ]; then
    export MESHCLIENT_CONSOLE="/dev/tty0"
fi
