#!/usr/bin/env bash
#
# The macOS download: MeshClient.app, universal (Apple silicon and Intel), inside a disk image.
#
#   scripts/package-macos.sh              # a development build of the bundle, for looking at
#   scripts/package-macos.sh 2.72.0       # a release: stamped, and allowed to update itself
#
# Writes to dist/:
#   MeshClient-macos.dmg              what a person downloads and drags to Applications
#   MeshClient-macos-app.zip          the same signed bundle as a `ditto` zip, which the in-app
#                                     updater downloads, unpacks beside the installed bundle and
#                                     swaps in whole. Not the executable alone: its signature
#                                     seals the bundle's Info.plist and resources, so a newer one
#                                     in an older bundle is a broken seal.
#   *.sha256                          `shasum -a 256` of each, in sha256sum's format
#
# SDL2 is built here from a pinned source release rather than taken from Homebrew, because
# Homebrew's is one architecture and a universal binary cannot link a thin library. It is bundled
# as a dylib in Contents/Frameworks, and an update carries it along with everything else.
#
# Signing is ad hoc (`codesign -s -`) unless MACOS_SIGN_IDENTITY names a Developer ID. Apple
# silicon will not run an unsigned binary at all, and ad hoc signing is enough for that. It is
# not enough for Gatekeeper, which blocks an unnotarised download on first launch until it is
# allowed under System Settings > Privacy & Security; README.md says so to the person installing.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${REPO_ROOT}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "scripts/package-macos.sh builds a macOS bundle and needs a Mac (codesign, hdiutil, lipo)." >&2
    exit 1
fi

VERSION="${1:-}"

# A download is built from this script's flags alone. A shell that exports a Homebrew
# toolchain's (LDFLAGS=-L/opt/homebrew/opt/llvm/lib is the usual one) hands the linker a
# single-architecture libunwind, and the x86_64 slice fails to link.
unset CFLAGS CXXFLAGS OBJCFLAGS CPPFLAGS LDFLAGS LIBRARY_PATH CPATH PKG_CONFIG_PATH

SDL_VERSION="2.32.10"
SDL_SHA256="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
ARCHS="${MESHCLIENT_MACOS_ARCHS:-arm64;x86_64}"
MIN_MACOS="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
SIGN_IDENTITY="${MACOS_SIGN_IDENTITY:--}"
ASSET_NAME="MeshClient-macos-app.zip"

BUILD_DIR="build/macos-release"
SDL_DIR="build/macos-sdl2-${SDL_VERSION}"
SDL_PREFIX="${PWD}/${SDL_DIR}/prefix"
DIST_DIR="dist"
APP="${DIST_DIR}/MeshClient.app"
DMG="${DIST_DIR}/MeshClient-macos.dmg"

# The bundle's version fields want numbers; a prerelease tag keeps its suffix in the binary,
# which is what the About screen and the updater read.
if [[ -n "${VERSION}" ]]; then
    NUMERIC="${VERSION%%-*}"
    NUMERIC="${NUMERIC%%+*}"
else
    NUMERIC=$(sed -n 's/^project(meshclient VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
fi
if [[ ! "${NUMERIC}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Cannot derive a numeric bundle version from '${VERSION:-CMakeLists.txt}'." >&2
    exit 1
fi

# actool compiles the .icon, and only Xcode 26's knows the format. xcrun's is whichever Xcode is
# selected, which on a CI image is not always the newest installed, so an installed Xcode 26 or
# later is looked for before giving up.
actool_major() {
    "$@" --version 2>/dev/null |
        awk -F'[<>.]' '/short-bundle-version/ { getline; print $3; exit }'
}
ACTOOL=(xcrun actool)
major=$(actool_major "${ACTOOL[@]}")
if [[ -z "${major}" || "${major}" -lt 26 ]]; then
    ACTOOL=()
    for xcode in $(ls -d /Applications/Xcode*.app 2>/dev/null | sort -rV); do
        candidate=(env DEVELOPER_DIR="${xcode}/Contents/Developer" xcrun actool)
        major=$(actool_major "${candidate[@]}")
        if [[ -n "${major}" && "${major}" -ge 26 ]]; then
            ACTOOL=("${candidate[@]}")
            break
        fi
    done
    if [[ ${#ACTOOL[@]} -eq 0 ]]; then
        echo "The app icon needs actool from Xcode 26 or later, and none is installed." >&2
        exit 1
    fi
fi

PYTHON=python3
if [[ -x .venv/bin/python3 ]]; then
    PYTHON="${PWD}/.venv/bin/python3"
fi

# ---------------------------------------------------------------------------------------------
# SDL2, universal, once per version. The prefix is the cache: CI keeps it between runs.
if [[ ! -f "${SDL_PREFIX}/lib/cmake/SDL2/SDL2Config.cmake" ]]; then
    mkdir -p "${SDL_DIR}"
    TARBALL="${SDL_DIR}/SDL2-${SDL_VERSION}.tar.gz"
    curl -fsSL -o "${TARBALL}" \
        "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL2-${SDL_VERSION}.tar.gz"
    echo "${SDL_SHA256}  ${TARBALL}" | shasum -a 256 -c -
    rm -rf "${SDL_DIR}/src"
    mkdir -p "${SDL_DIR}/src"
    tar -xzf "${TARBALL}" -C "${SDL_DIR}/src" --strip-components=1
    cmake -S "${SDL_DIR}/src" -B "${SDL_DIR}/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="${ARCHS}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${MIN_MACOS}" \
        -DCMAKE_INSTALL_PREFIX="${SDL_PREFIX}" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
    cmake --build "${SDL_DIR}/build"
    cmake --install "${SDL_DIR}/build"
fi

# ---------------------------------------------------------------------------------------------
# The client. CMAKE_PREFIX_PATH puts the SDL2 above ahead of Homebrew's in find_package().
CMAKE_ARGS=(
    -S . -B "${BUILD_DIR}" -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_OSX_ARCHITECTURES="${ARCHS}"
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MIN_MACOS}"
    -DCMAKE_PREFIX_PATH="${SDL_PREFIX}"
    -DPython3_EXECUTABLE="${PYTHON}"
    -DBUILD_TESTING=OFF
    -DMESHCLIENT_BUILD_DEVTOOLS=OFF
    # The asset this binary may replace itself with. Without it the updater's default applies,
    # which is the handheld's aarch64 Linux binary.
    -DMESHCLIENT_UPDATE_ASSET="${ASSET_NAME}"
)
# Both ways explicitly: the tree is reused, and a cached RELEASE_BUILD=ON left by a release
# would otherwise stamp the next development build as one.
if [[ -n "${VERSION}" ]]; then
    CMAKE_ARGS+=(-DMESHCLIENT_VERSION_OVERRIDE="${VERSION}" -DMESHCLIENT_RELEASE_BUILD=ON)
else
    CMAKE_ARGS+=(-DMESHCLIENT_VERSION_OVERRIDE= -DMESHCLIENT_RELEASE_BUILD=OFF)
fi

# shellcheck source=scripts/cmake-tree.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake-tree.sh"
mesh_reset_stale_tree "${BUILD_DIR}"
cmake "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --target meshclient

BINARY="${BUILD_DIR}/meshclient"
BUILT_ARCHS=" $(lipo -archs "${BINARY}") "
for arch in ${ARCHS//;/ }; do
    if [[ "${BUILT_ARCHS}" != *" ${arch} "* ]]; then
        echo "${BINARY} has no ${arch} slice (it has:${BUILT_ARCHS})." >&2
        exit 1
    fi
done
if [[ -n "${VERSION}" ]]; then
    VERSION_HITS=$(strings "${BINARY}" | grep -cF -- "${VERSION}" || true)
    if [[ "${VERSION_HITS}" -eq 0 ]]; then
        echo "${BINARY} does not contain the string ${VERSION}." >&2
        exit 1
    fi
fi

# ---------------------------------------------------------------------------------------------
# The bundle.
#
# The SDL2 the binary names is @rpath/libSDL2-2.0.0.dylib. Its build tree's rpath points at the
# prefix above, which no other Mac has, so that rpath is swapped for the bundle's own
# Frameworks directory.
SDL_LIB_NAME=$(otool -L "${BINARY}" | awk '/libSDL2/ && !found { print $1; found = 1 }')
if [[ "${SDL_LIB_NAME}" != @rpath/* ]]; then
    echo "Expected ${BINARY} to load SDL2 through @rpath, found '${SDL_LIB_NAME:-nothing}'." >&2
    exit 1
fi
SDL_LIB_FILE="${SDL_LIB_NAME#@rpath/}"

rm -rf "${APP}"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Frameworks" "${APP}/Contents/Resources"
sed -e "s/@VERSION@/${NUMERIC}/g" -e "s/@MIN_MACOS@/${MIN_MACOS}/g" \
    packaging/macos/Info.plist.in > "${APP}/Contents/Info.plist"
printf 'APPL????' > "${APP}/Contents/PkgInfo"
# The icon: an Icon Composer .icon compiled into Assets.car, which macOS 26 masks into its own
# squircle, and MeshClient.icns, which actool draws for macOS 11 to 15. An .icns on its own would
# be shown shrunk inside a grey tile on macOS 26. Info.plist names both (CFBundleIconName,
# CFBundleIconFile). The layer is drawn by scripts/gen-icons.py and committed.
# Absolute paths: actool does not resolve a relative one against this script's directory.
ICON_OUT="${REPO_ROOT}/${BUILD_DIR}/icon"
rm -rf "${ICON_OUT}"
mkdir -p "${ICON_OUT}"
"${ACTOOL[@]}" "${REPO_ROOT}/packaging/macos/MeshClient.icon" --compile "${ICON_OUT}" \
    --platform macosx --minimum-deployment-target "${MIN_MACOS}" --app-icon MeshClient \
    --output-partial-info-plist "${ICON_OUT}/partial.plist" --output-format human-readable-text
for f in Assets.car MeshClient.icns; do
    if [[ ! -s "${ICON_OUT}/${f}" ]]; then
        echo "actool did not write ${f} from packaging/macos/MeshClient.icon." >&2
        exit 1
    fi
    cp "${ICON_OUT}/${f}" "${APP}/Contents/Resources/${f}"
done
cp "${BINARY}" "${APP}/Contents/MacOS/meshclient"
cp -L "${SDL_PREFIX}/lib/${SDL_LIB_FILE}" "${APP}/Contents/Frameworks/${SDL_LIB_FILE}"
chmod 0644 "${APP}/Contents/Frameworks/${SDL_LIB_FILE}"

# The licences for what the binary carries (see scripts/package.sh), plus SDL2's, which the
# bundle carries as a file.
mkdir -p "${APP}/Contents/Resources/licenses"
cp LICENSE "${APP}/Contents/Resources/licenses/LICENSE-MeshClient.txt"
cp licenses/*.txt "${APP}/Contents/Resources/licenses/"
cp "${SDL_DIR}/src/LICENSE.txt" "${APP}/Contents/Resources/licenses/Zlib-SDL2.txt"

EXE="${APP}/Contents/MacOS/meshclient"
# `otool -l` on a universal binary lists every load command once per architecture, and one
# -delete_rpath removes a path from all of them - so each distinct path is deleted once.
while read -r rpath; do
    install_name_tool -delete_rpath "${rpath}" "${EXE}"
done < <(otool -l "${EXE}" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }' | sort -u)
install_name_tool -add_rpath "@executable_path/../Frameworks" "${EXE}"

# Inside out: the library, then the bundle, which signs its main executable in the bundle's
# context. The hardened runtime is only for a real identity: with an ad hoc signature it turns on
# library validation, which then refuses the bundled SDL2 for having no team.
SIGN_ARGS=(--force --timestamp=none --sign "${SIGN_IDENTITY}")
if [[ "${SIGN_IDENTITY}" != "-" ]]; then
    SIGN_ARGS=(--force --timestamp --options runtime --sign "${SIGN_IDENTITY}")
fi
codesign "${SIGN_ARGS[@]}" "${APP}/Contents/Frameworks/${SDL_LIB_FILE}"
codesign "${SIGN_ARGS[@]}" "${APP}"
codesign --verify --strict --verbose=2 "${APP}"

# The bundle as a Finder launch would load it: dyld resolving SDL2 through the new rpath is what
# this proves, and `--version` exits before any window or radio is involved.
"${EXE}" --version

# ---------------------------------------------------------------------------------------------
# The downloads.
#
# The update zip, made the way src/core/update/updater.c takes it apart (`ditto -x -k`), and
# taken apart here too: what the updater installs has to come out of the zip with its seal
# intact, and this is where that is proved rather than assumed. No resource forks, extended
# attributes or ACLs - a quarantine or provenance attribute from the build host is not something
# to ship.
rm -f "${DIST_DIR}/${ASSET_NAME}"
ditto -c -k --norsrc --noextattr --noacl --keepParent "${APP}" "${DIST_DIR}/${ASSET_NAME}"
ROUND_TRIP="${BUILD_DIR}/unzipped"
rm -rf "${ROUND_TRIP}"
ditto -x -k "${DIST_DIR}/${ASSET_NAME}" "${ROUND_TRIP}"
codesign --verify --strict --verbose=2 "${ROUND_TRIP}/MeshClient.app"
"${ROUND_TRIP}/MeshClient.app/Contents/MacOS/meshclient" --version

STAGE="${BUILD_DIR}/dmg"
rm -rf "${STAGE}" "${DMG}"
mkdir -p "${STAGE}"
cp -R "${APP}" "${STAGE}/"
ln -s /Applications "${STAGE}/Applications"
# hdiutil is known to fail with "Resource busy" on CI runners while Spotlight looks at the new
# volume. A retry is the documented workaround; three failures in a row is a real error.
for attempt in 1 2 3; do
    if hdiutil create -volname "MeshClient" -srcfolder "${STAGE}" -ov -format UDZO "${DMG}"; then
        break
    fi
    if [[ "${attempt}" -eq 3 ]]; then
        echo "hdiutil could not create ${DMG}." >&2
        exit 1
    fi
    sleep 5
done
if [[ "${SIGN_IDENTITY}" != "-" ]]; then
    codesign --force --timestamp --sign "${SIGN_IDENTITY}" "${DMG}"
fi

( cd "${DIST_DIR}" && shasum -a 256 MeshClient-macos.dmg > MeshClient-macos.dmg.sha256 )
( cd "${DIST_DIR}" && shasum -a 256 "${ASSET_NAME}" > "${ASSET_NAME}.sha256" )

echo "macOS ${VERSION:-development} build:"
ls -l "${DIST_DIR}/MeshClient-macos.dmg" "${DIST_DIR}/${ASSET_NAME}"
