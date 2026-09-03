# syntax=docker/dockerfile:1
#
# Container toolchains for mesh-client. The core is Linux-only (epoll/timerfd/eventfd),
# so on macOS all builds go through these images via scripts/docker.sh / make docker-*.
#
#   dev   - host-arch Debug/Release builds, unit tests, clang-format. Mirrors .github/workflows/ci.yml.
#   cross - dev + static aarch64 musl toolchain and static libdbus for producing MeshClient.pak.
#           Mirrors .github/workflows/semantic-release.yml (native musl-gcc on arm64 hosts,
#           Bootlin aarch64--musl toolchain on x86_64 hosts).

FROM ubuntu:24.04 AS dev
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang clang-format cmake ninja-build pkg-config \
        libdbus-1-dev protobuf-compiler python3 python3-pip \
        git zip ca-certificates \
    && python3 -m pip install --no-cache-dir --break-system-packages protobuf grpcio-tools \
    && rm -rf /var/lib/apt/lists/*
# Container builds write to build/linux/* so they never collide with a host CMake cache.
ENV BUILD_ROOT=build/linux
WORKDIR /src
CMD ["bash"]

FROM dev AS cross
COPY docker/setup-cross.sh /opt/cross/setup-cross.sh
RUN bash /opt/cross/setup-cross.sh
ENV PATH="/opt/cross/bin:${PATH}"
