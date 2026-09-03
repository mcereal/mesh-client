#!/usr/bin/env bash
# Install an aarch64 musl toolchain under /opt/cross and build static expat + libdbus against it.
# Produces /opt/cross/env.sh which scripts/cross-build.sh sources.
#
# arm64 host  : native musl-gcc (musl-tools) exposed as aarch64-linux-musl-{gcc,ar,ranlib}.
# x86_64 host : Bootlin aarch64--musl--stable toolchain (same as semantic-release.yml).
set -euo pipefail

PREFIX=/opt/cross
EXPAT_VERSION=2.5.0
DBUS_VERSION=1.14.10
BOOTLIN_TARBALL=aarch64--musl--stable-2024.02-1

mkdir -p "$PREFIX/bin" "$PREFIX/src"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends wget xz-utils bzip2 file

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
        ;;
    x86_64)
        cd "$PREFIX/src"
        wget -q "https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/${BOOTLIN_TARBALL}.tar.bz2"
        tar -xf "${BOOTLIN_TARBALL}.tar.bz2" -C "$PREFIX"
        rm -f "${BOOTLIN_TARBALL}.tar.bz2"
        for tool in gcc ar ranlib strip; do
            ln -sf "$PREFIX/$BOOTLIN_TARBALL/bin/aarch64-buildroot-linux-musl-$tool" "$PREFIX/bin/aarch64-linux-musl-$tool"
        done
        CROSS_COMPILE=aarch64-linux-musl-
        CROSS_HOST=aarch64-buildroot-linux-musl
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

# expat: only needed so dbus's configure/daemon build succeeds; libdbus-1 itself does not use it.
cd "$PREFIX/src"
wget -q "https://github.com/libexpat/libexpat/releases/download/R_${EXPAT_VERSION//./_}/expat-${EXPAT_VERSION}.tar.xz"
tar -xf "expat-${EXPAT_VERSION}.tar.xz"
cd "expat-${EXPAT_VERSION}"
./configure --host="$CROSS_HOST" CC="$CC" AR="$AR" RANLIB="$RANLIB" \
    --prefix="$PREFIX/expat" --disable-shared --enable-static --without-docbook >/dev/null
make -j"$JOBS" >/dev/null
make install >/dev/null

cd "$PREFIX/src"
wget -q "https://dbus.freedesktop.org/releases/dbus/dbus-${DBUS_VERSION}.tar.xz"
tar -xf "dbus-${DBUS_VERSION}.tar.xz"
cd "dbus-${DBUS_VERSION}"
PKG_CONFIG_PATH="$PREFIX/expat/lib/pkgconfig" \
./configure --host="$CROSS_HOST" CC="$CC" AR="$AR" RANLIB="$RANLIB" \
    --prefix="$PREFIX/dbus" --disable-shared --enable-static \
    --disable-tests --disable-doxygen-docs --disable-xml-docs --disable-selinux \
    --disable-systemd --disable-traditional-activation --without-x \
    EXPAT_CFLAGS="-I$PREFIX/expat/include" EXPAT_LIBS="-L$PREFIX/expat/lib -lexpat" >/dev/null
make -j"$JOBS" >/dev/null
make install >/dev/null

rm -rf "$PREFIX/src"/expat-* "$PREFIX/src"/dbus-*

cat > "$PREFIX/env.sh" <<ENV
# Sourced by scripts/cross-build.sh inside the cross container.
export PATH="$PREFIX/bin:\$PATH"
export CROSS_COMPILE="$CROSS_COMPILE"
export CROSS_HOST="$CROSS_HOST"
export CROSS_DBUS_PREFIX="$PREFIX/dbus"
export PKG_CONFIG_PATH="$PREFIX/dbus/lib/pkgconfig:\${PKG_CONFIG_PATH:-}"
export PLATFORM="\${PLATFORM:-tg5040}"
ENV

echo "cross toolchain ready:"
"$CC" --version | head -1
file "$PREFIX/dbus/lib/libdbus-1.a"
