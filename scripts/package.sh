#!/usr/bin/env bash
set -euo pipefail

BUILD_TYPE=${1:-release}
case "${BUILD_TYPE}" in
  debug|Debug)
    BUILD_SUBDIR=debug
    ;;
  release|Release)
    BUILD_SUBDIR=release
    ;;
  relwithdebinfo|RelWithDebInfo)
    BUILD_SUBDIR=relwithdebinfo
    ;;
  *)
    echo "Unknown build type: ${BUILD_TYPE}" >&2
    echo "Usage: $0 [debug|release|relwithdebinfo]" >&2
    exit 1
    ;;
esac

BUILD_ROOT="${BUILD_ROOT:-build}"
BUILD_DIR="${BUILD_ROOT}/${BUILD_SUBDIR}"
BINARY_PATH="${BUILD_DIR}/meshclient"
if [[ ! -x "${BINARY_PATH}" ]]; then
    echo "Binary not found at ${BINARY_PATH}. Run scripts/build.sh ${BUILD_TYPE} first." >&2
    exit 1
fi

DIST_DIR="dist"
OUTPUT_DIR="${DIST_DIR}/MeshClient.pak"
rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}/bin/shared"

cp "${BINARY_PATH}" "${OUTPUT_DIR}/bin/shared/meshclient"
chmod +x "${OUTPUT_DIR}/bin/shared/meshclient"

cp Tools/tg5040/MeshClient.pak/launch.sh "${OUTPUT_DIR}/launch.sh"
chmod +x "${OUTPUT_DIR}/launch.sh"

# pak.json rides inside the pak as well as sitting at the repo root: the root copy is what the
# Pak Store reads when it lists us, the packaged copy is how it knows which version is actually
# installed on the device. scripts/release-build.sh stamps both from the release tag.
cp pak.json "${OUTPUT_DIR}/pak.json"

# The CA bundle the updater verifies GitHub against. The Brick has no system CA store at all, so
# without this every HTTPS fetch fails with curl exit 60 and self-update cannot work; see the
# updater section of CLAUDE.md. It ships in the pak rather than through self-update, which is why
# a device that predates it has to reinstall the pak once to get updates working.
mkdir -p "${OUTPUT_DIR}/certs"
cp Tools/tg5040/MeshClient.pak/certs/certificates.crt "${OUTPUT_DIR}/certs/certificates.crt"

mkdir -p "${DIST_DIR}"
# The zip holds the *contents* of the pak, not the pak folder itself. That is what the NextUI
# Pak Store requires - it creates `Tools/<platform>/MeshClient.pak/` and unpacks into it, so a
# zip with the folder nested inside would install as MeshClient.pak/MeshClient.pak/launch.sh,
# a pak with no launcher. Installing by hand therefore means making the folder first; see the
# packaging section of README.md.
rm -f "${DIST_DIR}/MeshClient.pak.zip"
( cd "${OUTPUT_DIR}" && zip -qr "../MeshClient.pak.zip" . )

echo "Created package at ${DIST_DIR}/MeshClient.pak.zip"
