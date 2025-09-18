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

BUILD_DIR="build/${BUILD_SUBDIR}"
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

if [[ -d Tools/tg5040/MeshClient.pak/bin/shared ]]; then
    while IFS= read -r -d '' file; do
        dest="${OUTPUT_DIR}/bin/shared/$(basename "$file")"
        cp "$file" "$dest"
    done < <(find Tools/tg5040/MeshClient.pak/bin/shared -maxdepth 1 -type f ! -name '.gitkeep' -print0)
fi

if [[ -d Tools/tg5040/MeshClient.pak/bin/tg5040 ]]; then
    mkdir -p "${OUTPUT_DIR}/bin/tg5040"
    while IFS= read -r -d '' file; do
        dest="${OUTPUT_DIR}/bin/tg5040/$(basename "$file")"
        cp "$file" "$dest"
    done < <(find Tools/tg5040/MeshClient.pak/bin/tg5040 -maxdepth 1 -type f ! -name '.gitkeep' -print0)
fi

mkdir -p "${DIST_DIR}"
( cd "${DIST_DIR}" && zip -qr "MeshClient.pak.zip" "$(basename "${OUTPUT_DIR}")" )

echo "Created package at ${DIST_DIR}/MeshClient.pak.zip"
