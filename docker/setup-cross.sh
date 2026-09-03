#!/usr/bin/env bash
# Install an aarch64 musl toolchain under /opt/cross and build a static libdbus against it.
# Produces /opt/cross/env.sh which scripts/cross-build.sh sources.
#
# arm64 host  : native musl-gcc (musl-tools) exposed as aarch64-linux-musl-{gcc,ar,ranlib}.
# x86_64 host : Bootlin aarch64--musl--stable toolchain (same as semantic-release.yml).
set -euo pipefail

PREFIX=/opt/cross
DBUS_VERSION=1.16.2
BOOTLIN_TARBALL=aarch64--musl--stable-2025.08-1

mkdir -p "$PREFIX/bin" "$PREFIX/src"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends wget xz-utils bzip2 file meson ninja-build

ARCH="$(uname -m)"
case "$ARCH" in
    aarch64|arm64)
        apt-get install -y --no-install-recommends musl musl-dev musl-tools
        ln -sf /usr/bin/musl-gcc "$PREFIX/bin/aarch64-linux-musl-gcc"
        ln -sf /usr/bin/ar       "$PREFIX/bin/aarch64-linux-musl-ar"
        ln -sf /usr/bin/ranlib   "$PREFIX/bin/aarch64-linux-musl-ranlib"
        ln -sf /usr/bin/strip    "$PREFIX/bin/aarch64-linux-musl-strip"
        # musl-dev ships no kernel headers; expose the system ones (linux/fb.h, linux/input.h ...)
        # inside the musl include root. The uapi headers are libc-agnostic.
        MUSL_INC=/usr/include/aarch64-linux-musl
        ln -sfn /usr/include/linux "$MUSL_INC/linux"
        ln -sfn /usr/include/asm-generic "$MUSL_INC/asm-generic"
        ln -sfn "/usr/include/$(gcc -print-multiarch)/asm" "$MUSL_INC/asm"
        CROSS_COMPILE=aarch64-linux-musl-
        CROSS_HOST=aarch64-linux-musl
        # Ubuntu's libgcc.a outline-atomics init needs glibc's __getauxval, which musl lacks.
        # The Brick's Cortex-A53 has no LSE atomics, so inline atomics lose nothing.
        CROSS_CFLAGS="-mno-outline-atomics"
        ;;
    x86_64)
        cd "$PREFIX/src"
        wget -q "https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/${BOOTLIN_TARBALL}.tar.xz"
        tar -xf "${BOOTLIN_TARBALL}.tar.xz" -C "$PREFIX"
        rm -f "${BOOTLIN_TARBALL}.tar.xz"
        for tool in gcc ar ranlib strip; do
            ln -sf "$PREFIX/$BOOTLIN_TARBALL/bin/aarch64-buildroot-linux-musl-$tool" "$PREFIX/bin/aarch64-linux-musl-$tool"
        done
        CROSS_COMPILE=aarch64-linux-musl-
        CROSS_HOST=aarch64-buildroot-linux-musl
        CROSS_CFLAGS=""
        ;;
    *)
        echo "Unsupported container architecture: $ARCH" >&2
        exit 1
        ;;
esac
export PATH="$PREFIX/bin:$PATH"

CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
RANLIB="${CROSS_COMPILE}ranlib"
JOBS="$(nproc)"

# libdbus only (message_bus=false skips the daemon, so no XML parser is needed). dbus >= 1.16 is meson-only.
cat > "$PREFIX/meson-cross.ini" <<INI
[binaries]
c = '${CROSS_COMPILE}gcc'
ar = '${CROSS_COMPILE}ar'
strip = '${CROSS_COMPILE}strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = [$(for f in $CROSS_CFLAGS; do printf "'%s', " "$f"; done)]

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
INI

cd "$PREFIX/src"
wget -q "https://dbus.freedesktop.org/releases/dbus/dbus-${DBUS_VERSION}.tar.xz"
tar -xf "dbus-${DBUS_VERSION}.tar.xz"
cd "dbus-${DBUS_VERSION}"
meson setup build --cross-file "$PREFIX/meson-cross.ini" \
    --prefix="$PREFIX/dbus" --libdir=lib --buildtype=release \
    -Ddefault_library=static -Dmessage_bus=false -Dtools=false \
    -Dmodular_tests=disabled -Dintrusive_tests=false -Dinstalled_tests=false \
    -Dxml_docs=disabled -Ddoxygen_docs=disabled -Dducktype_docs=disabled -Dqt_help=disabled \
    -Dselinux=disabled -Dapparmor=disabled -Dsystemd=disabled -Dlibaudit=disabled \
    -Dx11_autolaunch=disabled >/dev/null
ninja -C build >/dev/null
ninja -C build install >/dev/null

rm -rf "$PREFIX/src"/dbus-*

cat > "$PREFIX/env.sh" <<ENV
# Sourced by scripts/cross-build.sh inside the cross container.
export PATH="$PREFIX/bin:\$PATH"
export CROSS_COMPILE="$CROSS_COMPILE"
export CROSS_HOST="$CROSS_HOST"
export CROSS_CFLAGS="$CROSS_CFLAGS"
export CROSS_DBUS_PREFIX="$PREFIX/dbus"
export PKG_CONFIG_PATH="$PREFIX/dbus/lib/pkgconfig:\${PKG_CONFIG_PATH:-}"
export PLATFORM="\${PLATFORM:-tg5040}"
ENV

echo "cross toolchain ready:"
"$CC" --version | head -1
file "$PREFIX/dbus/lib/libdbus-1.a"
