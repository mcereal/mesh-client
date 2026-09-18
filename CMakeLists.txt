# 3.18 is what this file needs; the *workflow* wants 3.21, which is where CMakePresets.json
# version 3 lands. Deliberately not raised to match: presets are a command-line feature, and
# nothing here is written against a policy newer than this. A too-old cmake is turned away by
# the presets file itself, which says so by name.
cmake_minimum_required(VERSION 3.18)
project(meshclient VERSION 2.70.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MESHCLIENT_ENABLE_ASAN "Enable AddressSanitizer for debugging" OFF)
option(MESHCLIENT_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for debugging" OFF)

# Every target here - the core, the client, the tests, the devtools, the fuzz harnesses -
# compiles under the same warnings and links under the same sanitizers. Carrying that as a
# target rather than as a pair of variables is what keeps the two halves of a sanitizer
# together: -fsanitize=address has to reach the compile *and* the link, and a variable holding
# one of them can be applied to a new target without the other. An INTERFACE library cannot be
# half-applied, and an empty one contributes nothing - which is also what retires the
# `if(MESHCLIENT_LINK_OPTIONS)` guard that used to precede every target_link_options() call.
#
# Always linked PRIVATE: this is how this project compiles, not something a consumer inherits.
add_library(meshclient_options INTERFACE)

target_compile_options(meshclient_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wcast-align
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wformat=2
)

if(MESHCLIENT_ENABLE_ASAN)
    target_compile_options(meshclient_options INTERFACE -fsanitize=address)
    target_link_options(meshclient_options INTERFACE -fsanitize=address)
endif()

# The harnesses in devtools/fuzz/ need two halves of libFuzzer, and this is the half that goes
# on the code under test: `fuzzer-no-link` instruments it for coverage without giving it a
# main(). The harness targets add plain `-fsanitize=fuzzer` at their own link step, which is
# what supplies one. Without the instrumentation here the fuzzer would be feeling its way
# through the parser with no coverage feedback at all - a random byte generator with extra steps.
option(MESHCLIENT_ENABLE_FUZZERS "Build the libFuzzer harnesses in devtools/fuzz (clang only)" OFF)
if(MESHCLIENT_ENABLE_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
                "MESHCLIENT_ENABLE_FUZZERS needs clang for libFuzzer; configure with CC=clang.")
    endif()
    target_compile_options(meshclient_options INTERFACE -fsanitize=fuzzer-no-link)
endif()

# -fno-sanitize-recover because UBSan otherwise prints a diagnostic and carries on, so the
# test that found the undefined behaviour still passes and CI still goes green. A check nobody
# fails is a check nobody reads.
if(MESHCLIENT_ENABLE_UBSAN)
    target_compile_options(meshclient_options INTERFACE
                           -fsanitize=undefined -fno-sanitize-recover=undefined)
    target_link_options(meshclient_options INTERFACE -fsanitize=undefined)
endif()

add_library(meshclient_core
    src/app/app.c
    src/app/app_actions.c
    src/app/app_mqtt.c
    src/app/app_publish.c
    src/app/app_settings.c
    src/core/session/channel_share.c
    src/core/session/contact_share.c
    src/core/runtime/config.c
    src/core/firmware/esp_image.c
    src/core/runtime/event_loop.c
    src/core/net/fetch.c
    src/core/firmware/firmware.c
    src/core/firmware/firmware_catalog.c
    src/core/firmware/firmware_download.c
    src/core/firmware/firmware_fetch.c
    src/core/firmware/firmware_install.c
    src/core/firmware/firmware_ota.c
    src/core/firmware/firmware_update.c
    src/core/session/key_verification.c
    src/core/session/message.c
    src/core/session/waypoint.c
    src/core/session/radio_settings.c
    src/core/net/mqtt_proxy.c
    src/core/net/resolve.c
    src/core/session/session.c
    src/core/runtime/signals.c
    src/core/session/store_forward.c
    src/core/net/tls_client.c
    src/core/firmware/uf2.c
    src/core/update/updater.c
    src/core/update/version.c
    src/transport/transport_registry.c
    src/transport/stream_link.c
    src/transport/ble/ble_hci.c
    src/transport/ble/ble_ota.c
    src/transport/ble/ble_transport.c
    src/transport/ble/bluez_client.c
    src/transport/serial/serial_transport.c
    src/transport/serial/serial_usb.c
    src/transport/serial/usb_msc.c
    src/transport/tcp/tcp_transport.c
    src/proto/channel_url.c
    src/proto/contact_url.c
    src/proto/mqtt_packet.c
    src/proto/mqtt_topic.c
    src/proto/stream_framing.c
    src/geo/coords.c
    src/geo/mercator.c
    src/geo/vector.c
    src/map/source_pack.c
    src/map/tile.c
    src/map/tile_cache.c
    src/map/tile_image.c
    src/map/viewport.c
    src/map/wuffs_png.c
    src/i18n/strings.c
    src/i18n/locale_es.c
    src/utils/base64.c
    src/utils/crash.c
    src/utils/env.c
    src/utils/file.c
    src/utils/json.c
    src/utils/log.c
    src/utils/qr.c
    src/utils/sha256.c
    src/utils/text.c
    src/utils/time.c
    src/utils/zip.c
    src/ui/tables/actions.c
    src/ui/views/channel_share.c
    src/ui/views/contact_share.c
    src/ui/tables/chrome.c
    src/ui/tables/help.c
    src/ui/tables/delivery.c
    src/ui/tables/duration.c
    src/ui/anim.c
    src/ui/store/store_archive.c
    src/ui/store/store.c
    src/ui/store/store_fields.c
    src/ui/store/store_file.c
    src/ui/store/store_keys.c
    src/ui/store/store_trends.c
    src/ui/store/history.c
    src/ui/nav/controller.c
    src/ui/input/input.c
    src/ui/input/input_profile.c
    src/ui/views/latency.c
    src/ui/views/map.c
    src/ui/nav/nav.c
    src/ui/nav/nav_canned.c
    src/ui/nav/nav_conversations.c
    src/ui/nav/nav_keyboard.c
    src/ui/nav/nav_map.c
    src/ui/nav/nav_settings.c
    src/ui/nav/nav_waypoints.c
    src/ui/tables/reactions.c
    src/ui/settings/settings.c
    src/ui/settings/settings_codec.c
    src/ui/settings/settings_rows.c
    src/ui/nav/route.c
    src/ui/tables/status.c
    src/ui/views/node_detail.c
    src/ui/tables/devices.c
    src/ui/tables/nodes.c
    src/ui/tables/units.c
    src/ui/views/waypoints.c
    src/ui/backends/stub.c
    src/ui/backends/cli.c
    src/ui/backends/fb.c
    src/ui/backends/fb_capture.c
    src/ui/backends/fb_draw.c
    src/ui/backends/fb_map.c
    src/ui/backends/fb_widgets_bubble.c
    src/ui/backends/fb_widgets_button.c
    src/ui/backends/fb_widgets_card.c
    src/ui/backends/fb_widgets_chrome.c
    src/ui/backends/fb_widgets_control.c
    src/ui/backends/fb_widgets_item.c
    src/ui/backends/fb_widgets_list.c
    src/ui/backends/fb_widgets_meter.c
    src/ui/backends/fb_widgets_overlay.c
    src/ui/backends/fb_screens_chart.c
    src/ui/backends/fb_screens_code.c
    src/ui/backends/fb_screens_compose.c
    src/ui/backends/fb_screens_devices.c
    src/ui/backends/fb_screens_frame.c
    src/ui/backends/fb_screens_messages.c
    src/ui/backends/fb_screens_nodes.c
    src/ui/backends/fb_screens_overlays.c
    src/ui/backends/fb_screens_settings.c
    src/ui/backends/fb_screens_status.c
    src/ui/backends/fb_screens_waypoints.c
    src/ui/theme/font.c
    src/ui/theme/font5x7.c
    src/ui/theme/font_ui.c
    src/ui/generated/font_ui_glyphs.c
    src/ui/theme/theme.c
    src/ui/theme/emoji.c
    src/ui/generated/emoji_glyphs.c
    src/ui/theme/icon.c
    src/ui/generated/icon_glyphs.c
    src/ui/store/preferences.c
    src/ui/layout.c
    src/ui/tables/trend.c
    src/ui/tables/trust.c
)

target_include_directories(meshclient_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

# The client's own version, for the About screen and the updater.
#
# Two knobs rather than one, because PROJECT_VERSION cannot carry the whole answer:
#
#  - project(VERSION) only accepts numeric components - CMake errors outright on a SemVer
#    prerelease like 1.13.0-beta.1 - but the beta and rc channels release exactly those. So the
#    project() line stays numeric and scripts/release-build.sh passes the full tag separately.
#  - a build is only a *release* when the release script says so. Stamping PROJECT_VERSION
#    unconditionally would make every `make debug` and `make brick` binary claim to be an
#    official release, and the updater would then happily replace a developer's own build with
#    whatever is on GitHub. Local builds report "<version>-dev" and are never updatable.
# The version the binary reports. project(VERSION) by default; MESHCLIENT_VERSION_OVERRIDE only
# exists for release-build.sh, which has to pass the whole tag because project(VERSION) accepts
# numeric components only and cannot hold a "-beta.1" suffix.
#
# The override defaults to empty rather than to PROJECT_VERSION, and that is the whole point.
# Seeding a cache entry with the project version freezes it: `set(... CACHE STRING)` writes on
# the *first* configure of a build tree and never again, so an existing build/ went on reporting
# the version it was created at. Every incremental `make debug` / `make brick` between 1.13.0 and
# 1.16.0 stamped 1.13.0-dev, and the About screen and the updater's version comparison believed
# it. An empty default cannot go stale, so a bump always takes - including in build trees that
# already exist, since nothing has ever written this name.
set(MESHCLIENT_VERSION_OVERRIDE "" CACHE STRING
    "Full SemVer version to report, prerelease suffix included; empty means project(VERSION)")
if(MESHCLIENT_VERSION_OVERRIDE)
    set(MESHCLIENT_VERSION_FULL "${MESHCLIENT_VERSION_OVERRIDE}")
else()
    set(MESHCLIENT_VERSION_FULL "${PROJECT_VERSION}")
endif()
option(MESHCLIENT_RELEASE_BUILD "Stamp this build as an official release" OFF)

if(MESHCLIENT_RELEASE_BUILD)
    target_compile_definitions(meshclient_core PUBLIC
        MESHCLIENT_VERSION="${MESHCLIENT_VERSION_FULL}"
        MESHCLIENT_RELEASE_BUILD=1)
else()
    target_compile_definitions(meshclient_core PUBLIC
        MESHCLIENT_VERSION="${MESHCLIENT_VERSION_FULL}-dev")
endif()

# Which release asset this build replaces itself with. src/core/update/updater.c defaults to the
# handheld's, because the device is the build self-update was written for - and that default is
# only right for a binary published under that name. A release carries more than one now, so a
# build published as something else has to say which is *its* asset: the alternative is a
# desktop client that checks for an update, finds one, and atomically installs an aarch64
# handheld binary over itself. The current process survives that; the next launch does not.
#
# Empty means the updater's own default, which keeps every existing build - the pak, and any
# configure that does not name this - exactly as it was. MESHCLIENT_UPDATE_ASSET in the
# environment still overrides both at runtime.
set(MESHCLIENT_UPDATE_ASSET "" CACHE STRING
    "Release asset this build self-updates from; empty means the updater's own default")
if(MESHCLIENT_UPDATE_ASSET)
    target_compile_definitions(meshclient_core PUBLIC
        MESHCLIENT_UPDATE_ASSET="${MESHCLIENT_UPDATE_ASSET}")
endif()

target_link_libraries(meshclient_core PRIVATE meshclient_options)

# Wuffs, for the map's PNG decoder. See third_party/wuffs/README.md for what it is and why it is
# a vendored file rather than a submodule; src/map/wuffs_png.h is what keeps the thirty codecs
# this client does not want out of the binary.
#
# Only src/map/wuffs_png.c compiles it, and it is the one source in the tree built without this
# project's warning flags: 3.6 MB of generated C has no business answering to -Wconversion, and
# -w after the target's options is what turns them off for that file alone. Sanitizers are
# deliberately *not* turned off with them - a decoder is exactly what they are for, and one that
# claims to be memory-safe by construction is exactly the claim worth checking.
target_include_directories(meshclient_core PRIVATE ${PROJECT_SOURCE_DIR}/third_party/wuffs)
set_source_files_properties(src/map/wuffs_png.c PROPERTIES COMPILE_OPTIONS "-w")

find_package(Threads REQUIRED)
find_package(PkgConfig)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(NANOPB_GENERATOR_EXECUTABLE nanopb_generator)

if(NANOPB_GENERATOR_EXECUTABLE)
    set(NANOPB_GENERATOR_COMMAND ${NANOPB_GENERATOR_EXECUTABLE})
else()
    set(NANOPB_GENERATOR_COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/third_party/nanopb/generator/nanopb_generator.py)
endif()

if(PkgConfig_FOUND)
    pkg_check_modules(DBUS dbus-1)
endif()

if(DBUS_FOUND)
    target_compile_definitions(meshclient_core PRIVATE MESH_HAVE_DBUS)
    target_include_directories(meshclient_core PRIVATE ${DBUS_INCLUDE_DIRS})
    target_link_libraries(meshclient_core PUBLIC ${DBUS_LIBRARIES})
endif()


# libm, for the two files that need it, both in src/geo: vector.c turns two coordinates into a
# distance and a bearing, and mercator.c turns one into a position on a flat picture of the
# world. Nothing outside that directory includes <math.h> - the UI is fixed-point throughout -
# and this link was anticipated when the geo module was proposed.
#
# The bare name rather than find_library(m), and that is the whole of the reason it is written
# this way: a bare name reaches the linker as `-lm` and is resolved by the compiler driver
# against its own sysroot, while find_library() searches the *host* and hands back an absolute
# path. scripts/cross-build.sh sets CMAKE_SYSTEM_NAME and the compiler but no
# CMAKE_FIND_ROOT_PATH, so on the x86_64 runner that path is /usr/lib/x86_64-linux-gnu/libm.so -
# an x86 library passed to an aarch64 static link, which fails at the one step nothing else in
# this build would have caught.
target_link_libraries(meshclient_core
    PUBLIC
        Threads::Threads
        m
)

add_executable(meshclient src/main.c)

target_link_libraries(meshclient PRIVATE meshclient_core meshclient_options)

set(nanopb_BUILD_GENERATOR OFF CACHE BOOL "Disable nanopb generator build" FORCE)
set(nanopb_BUILD_RUNTIME ON CACHE BOOL "Enable nanopb runtime" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Use static nanopb" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build static nanopb" FORCE)

add_subdirectory(third_party/nanopb)
target_link_libraries(meshclient_core PUBLIC protobuf-nanopb-static)

# Mbed TLS, for the one connection this process has to encrypt itself: MQTT to a broker, on
# behalf of a radio that cannot reach one. Everything else it fetches over HTTPS is a forked
# curl (src/core/net/fetch.c), and a broker connection cannot be - it is long-lived, bidirectional
# and has to sit on the same epoll loop as the UI. See include/mesh/core/tls_client.h.
#
# Optional on purpose. `MESHCLIENT_HAVE_TLS` is what src/core/net/tls_client.c switches on, and
# without the submodule it compiles to a stub that reports TLS unavailable - the same
# arrangement MESH_HAVE_DBUS gives the BLE transport. A checkout that skipped
# `git submodule update --recursive` still builds and still runs; MQTT over TLS is the one thing
# it cannot do, and it says so rather than failing to link.
if(EXISTS ${PROJECT_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt)
    # Mbed TLS builds its own tests, sample programs and package config by default, none of which
    # belong in this build. FATAL_WARNINGS is turned off because it is that project's policy
    # about its own warnings, applied with this project's compiler flags - a combination neither
    # side tested.
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    # 4.x generates error.c, the SSL debug helpers and the PSA driver wrappers at build time;
    # unlike 3.6 they are not committed to the submodule. Needs Python with jinja2 - see
    # scripts/setup.sh and docker/.
    set(GEN_FILES ON CACHE BOOL "" FORCE)
    set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)

    # Where this build's two config files live. Mbed TLS 4.x is two projects - TLS/X.509 here,
    # everything cryptographic in the vendored tf-psa-crypto - and each reads its own user config
    # file. The library applies both to its own targets as PUBLIC compile definitions, so unlike
    # 3.x there is nothing to set by hand afterwards and no include directory to add: the value
    # is the full path the generated `#include` uses.
    set(MBEDTLS_USER_CONFIG_FILE
        ${PROJECT_SOURCE_DIR}/third_party/mbedtls-config/mesh_mbedtls_config.h
        CACHE FILEPATH "" FORCE)
    set(TF_PSA_CRYPTO_USER_CONFIG_FILE
        ${PROJECT_SOURCE_DIR}/third_party/mbedtls-config/mesh_psa_crypto_config.h
        CACHE FILEPATH "" FORCE)

    add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)

    target_compile_definitions(meshclient_core PUBLIC MESHCLIENT_HAVE_TLS)
    # `tfpsacrypto` is 4.x's name for what 3.x called `mbedcrypto`.
    target_link_libraries(meshclient_core PUBLIC mbedtls mbedx509 tfpsacrypto)
else()
    message(STATUS
        "third_party/mbedtls is missing: building without TLS, so MQTT will be plaintext only. "
        "Run `git submodule update --init --recursive` to enable it.")
endif()

set(NANOPB_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated/nanopb)
file(MAKE_DIRECTORY ${NANOPB_GENERATED_DIR})

set(MESH_PROTO_ROOT ${PROJECT_SOURCE_DIR}/proto/meshtastic)
if(NOT EXISTS ${MESH_PROTO_ROOT}/meshtastic)
    message(FATAL_ERROR "Meshtastic protobuf submodule missing. Run 'git submodule update --init --recursive'.")
endif()

set(MESH_PROTO_NAMES
    mesh
    portnums
    interdevice
    config
    module_config
    telemetry
    channel
    apponly
    device_ui
    xmodem
    atak
    admin
    connection_status
    field_metadata
    storeforward
)

set(MESH_PROTO_FILES)
foreach(PROTO_NAME ${MESH_PROTO_NAMES})
    set(PROTO_PATH ${MESH_PROTO_ROOT}/meshtastic/${PROTO_NAME}.proto)
    if(NOT EXISTS ${PROTO_PATH})
        message(FATAL_ERROR "Expected proto not found: ${PROTO_PATH}")
    endif()
    list(APPEND MESH_PROTO_FILES ${PROTO_PATH})
endforeach()

set(NANOPB_GENERATED_SOURCES)
set(NANOPB_GENERATED_HEADERS)

# Where nanopb_generator.py is allowed to build its copy of nanopb_pb2.py, and the reason this
# is set at all.
#
# The generator needs a python-protobuf build of nanopb.proto, which upstream does not commit.
# Left alone it generates one and caches it *in the submodule*, at
# third_party/nanopb/generator/proto/nanopb_pb2.py. One command below per entry in
# MESH_PROTO_NAMES, all independent, so ninja starts them together - and on a tree where that
# cache does not exist yet, they all try to create it at the same path at once. A generator that
# imports the file mid-write gets a truncated module rather than an error, and dies on the first
# attribute it reads:
#
#   AttributeError: module 'proto.nanopb_pb2' has no attribute 'IS_8'
#
# Reproduced 8 times out of 8 by deleting the cache and running the fifteen invocations
# concurrently; 0 out of 8 with this set. It needs a fresh checkout to bite, which is every CI
# run and almost no local one - so it reads as an unrelated flake in whatever pull request it
# lands on.
#
# NANOPB_PB2_TEMP_DIR is upstream's switch for exactly this: it leaves the *read* of an existing
# cache alone and only stops the shared write, so each invocation that has to build the module
# builds its own private copy under this directory instead. Measured at no cost either way -
# 0.4s for all fifteen, cached or fresh - because the builds overlap.
set(NANOPB_PB2_TEMP_DIR ${CMAKE_BINARY_DIR}/nanopb-pb2)
file(MAKE_DIRECTORY ${NANOPB_PB2_TEMP_DIR})

foreach(PROTO_FILE ${MESH_PROTO_FILES})
    get_filename_component(PROTO_BASENAME ${PROTO_FILE} NAME_WE)
    get_filename_component(PROTO_DIR ${PROTO_FILE} DIRECTORY)
    file(RELATIVE_PATH PROTO_SUBDIR ${MESH_PROTO_ROOT} ${PROTO_DIR})
    if(PROTO_SUBDIR STREQUAL "")
        set(OUTPUT_SUBDIR "")
    else()
        set(OUTPUT_SUBDIR ${PROTO_SUBDIR})
    endif()

    set(GENERATED_DIR ${NANOPB_GENERATED_DIR}/${OUTPUT_SUBDIR})
    set(GENERATED_C ${GENERATED_DIR}/${PROTO_BASENAME}.pb.c)
    set(GENERATED_H ${GENERATED_DIR}/${PROTO_BASENAME}.pb.h)

    add_custom_command(
        OUTPUT ${GENERATED_C} ${GENERATED_H}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED_DIR}
        COMMAND ${CMAKE_COMMAND} -E env
            NANOPB_PB2_TEMP_DIR=${NANOPB_PB2_TEMP_DIR}
            ${NANOPB_GENERATOR_COMMAND}
            -D ${NANOPB_GENERATED_DIR}
            -I ${MESH_PROTO_ROOT}
            -I ${MESH_PROTO_ROOT}/meshtastic
            ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "Generating nanopb sources for ${PROTO_FILE}"
        VERBATIM
    )

    list(APPEND NANOPB_GENERATED_SOURCES ${GENERATED_C})
    list(APPEND NANOPB_GENERATED_HEADERS ${GENERATED_H})
endforeach()

set_source_files_properties(${NANOPB_GENERATED_SOURCES} ${NANOPB_GENERATED_HEADERS} PROPERTIES GENERATED TRUE)

add_custom_target(nanopb_codegen DEPENDS ${NANOPB_GENERATED_SOURCES} ${NANOPB_GENERATED_HEADERS})
add_dependencies(meshclient_core nanopb_codegen)
target_sources(meshclient_core PRIVATE ${NANOPB_GENERATED_SOURCES})
target_include_directories(meshclient_core PUBLIC ${NANOPB_GENERATED_DIR})

# Host-only development tools (devtools/README.md): they render the UI off-screen so a change
# can be looked at without a Brick. Nothing in the pak needs them, and the cross build cannot
# run what it produces, so it skips them by default.
if(CMAKE_CROSSCOMPILING)
    set(MESHCLIENT_DEVTOOLS_DEFAULT OFF)
else()
    set(MESHCLIENT_DEVTOOLS_DEFAULT ON)
endif()
option(MESHCLIENT_BUILD_DEVTOOLS "Build the host-side development tools in devtools/"
       ${MESHCLIENT_DEVTOOLS_DEFAULT})
if(MESHCLIENT_BUILD_DEVTOOLS)
    add_subdirectory(devtools)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS meshclient RUNTIME DESTINATION bin)
