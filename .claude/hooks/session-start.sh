#!/bin/bash
# SessionStart hook: provision a Claude Code on the web session for building mesh-client.
#
# The core is Linux-only, so a cloud session can build and test natively (no Docker needed):
# it only needs the git submodules, libdbus-1-dev and the Python protobuf packages that
# docker/Dockerfile installs for the dev image. scripts/setup-linux.sh does exactly that
# and is safe to re-run.
set -euo pipefail

# Local machines are already set up the way the developer wants (macOS goes through
# scripts/docker.sh); only provision the remote container.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
    exit 0
fi

cd "${CLAUDE_PROJECT_DIR:-$(dirname "$0")/../..}"

./scripts/setup-linux.sh

# The container is Linux, so `make debug`/`make test` work directly against build/.
# Recorded here so the session does not reach for the docker-* targets by habit.
if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
    echo 'export MESHCLIENT_NATIVE_LINUX=1' >> "$CLAUDE_ENV_FILE"
fi
