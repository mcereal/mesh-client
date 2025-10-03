#!/bin/sh
set -eu

# Helper script to run meshclient in background so launch.sh can control the TTY
meshclient --foreground "$@" &
CLIENT_PID=$!
trap 'kill "$CLIENT_PID" 2>/dev/null; wait "$CLIENT_PID" 2>/dev/null' INT TERM
wait "$CLIENT_PID"
