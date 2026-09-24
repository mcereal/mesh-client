# 3.18 is what this file needs; the *workflow* wants 3.21, which is where CMakePresets.json
# version 3 lands. Deliberately not raised to match: presets are a command-line feature, and
# nothing here is written against a policy newer than this. A too-old cmake is turned away by
# the presets file itself, which says so by name.
cmake_minimum_required(VERSION 3.18)
project(meshclient VERSION 2.71.1 LANGUAGES C)

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
    src/core/firmware/firmware.c
    src/core/firmware/firmware_catalog.c
    src/core/firmware/firmware_fetch.c
    src/core/firmware/firmware_install.c
    src/core/firmware/firmware_ota.c
    src/core/firmware/firmware_update.c
    src/core/session/key_verification.c
    src/core/session/message.c
    src/core/session/waypoint.c
    src/core/session/radio_settings.c
    src/core/session/session.c
    src/core/session/store_forward.c
    src/core/firmware/uf2.c
    src/core/update/updater.c
    src/core/update/version.c
    src/transport/transport_registry.c
    src/transport/stream_link.c
    src/transport/ble/ble_hci.c
    src/transport/ble/ble_ota.c
    src/transport/ble/ble_transport.c
    src/transport/ble/ble_gatt.c
    src/transport/serial/serial_transport.c
    src/transport/serial/serial_usb.c
    src/transport/tcp/tcp_transport.c
    src/proto/channel_url.c
    src/proto/contact_url.c
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
    src/utils/crash.c
    src/utils/file.c
    src/i18n/net_reason.c
    src/i18n/strings.c
    src/i18n/locale_es.c
    src/ui/tables/mqtt.c
    src/ui/tables/trend.c
    src/ui/tables/actions.c
    src/ui/views/channel_share.c
    src/ui/views/contact_share.c
    src/ui/tables/chrome.c
    src/ui/tables/help.c
    src/ui/tables/delivery.c
    src/ui/tables/duration.c
    src/ui/store/store_archive.c
    src/ui/store/store.c
    src/ui/store/store_fields.c
    src/ui/store/store_file.c
    src/ui/store/store_keys.c
    src/ui/store/store_trends.c
    src/ui/store/history.c
    src/ui/nav/controller.c
    src/ui/views/map.c
    src/ui/nav/nav.c
    src/ui/nav/nav_canned.c
    src/ui/nav/nav_click.c
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
    src/ui/backends/fb_app.c
    src/ui/backends/fb_map.c
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
    src/ui/store/preferences.c
    src/ui/tables/trust.c
)

# Keep the host policy at the build boundary. Shared app code only consumes the selected
# default; it does not need a growing list of platform conditionals.
if(WIN32)
    set(MESHCLIENT_DEFAULT_UI_BACKEND sdl)
else()
    set(MESHCLIENT_DEFAULT_UI_BACKEND fb)
endif()
target_compile_definitions(meshclient_core PRIVATE
    MESHCLIENT_DEFAULT_UI_BACKEND="${MESHCLIENT_DEFAULT_UI_BACKEND}")

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

# inkwell as well as inkcell, and PUBLIC for the same reason inkcell is: this client's own
# public headers hand out inkwell types - an inkwell_loop is in half the init signatures here -
# so anything compiling against mesh/ needs inkwell's include path too.
target_link_libraries(meshclient_core PUBLIC inkcell::inkcell inkwell::inkwell)
target_link_libraries(meshclient_core PRIVATE meshclient_options)


find_package(Threads REQUIRED)
find_package(PkgConfig)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(NANOPB_GENERATOR_EXECUTABLE nanopb_generator)

if(NANOPB_GENERATOR_EXECUTABLE)
    set(NANOPB_GENERATOR_COMMAND ${NANOPB_GENERATOR_EXECUTABLE})
else()
    set(NANOPB_GENERATOR_COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/third_party/nanopb/generator/nanopb_generator.py)
endif()

# BlueZ, and the D-Bus it is reached over, are inkwell's: `inkwell/ble/central.h` is the
# Bluetooth stack, and inkwell decides at configure time whether it has one.


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
)
if(WIN32)
    # BCryptGenRandom is the Windows system CSPRNG used for newly generated channel keys.
    target_link_libraries(meshclient_core PRIVATE bcrypt)
else()
    target_link_libraries(meshclient_core PUBLIC m)
endif()

add_executable(meshclient src/main.c)

target_link_libraries(meshclient PRIVATE meshclient_core meshclient_options)

# Windows: the application manifest, whose job is to make the process's ANSI code page UTF-8 so
# that a path outside the system's legacy code page survives every char-based file call. See
# src/app/meshclient.manifest.
if(WIN32)
    enable_language(RC)
    set(MESHCLIENT_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/src/app/meshclient.manifest)
    set(MESHCLIENT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/meshclient.ico)
    configure_file(src/app/meshclient.rc.in ${CMAKE_CURRENT_BINARY_DIR}/meshclient.rc @ONLY)
    target_sources(meshclient PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/meshclient.rc)
    set_property(TARGET meshclient APPEND PROPERTY LINK_DEPENDS ${MESHCLIENT_MANIFEST} ${MESHCLIENT_ICON})
endif()

# A bare binary on macOS has no bundle, so the Info.plist that says why it wants Bluetooth is
# linked into its __TEXT segment instead. CoreBluetooth is refused without a usage string, and
# macOS asks the user about the app that launched the binary - Terminal, usually - with this
# sentence in the dialog.
if(APPLE)
    configure_file(src/app/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
    target_link_options(meshclient PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
    set_property(TARGET meshclient APPEND PROPERTY LINK_DEPENDS
                 ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
endif()

set(nanopb_BUILD_GENERATOR OFF CACHE BOOL "Disable nanopb generator build" FORCE)
set(nanopb_BUILD_RUNTIME ON CACHE BOOL "Enable nanopb runtime" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Use static nanopb" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build static nanopb" FORCE)

# The UI toolkit this client was built out of. Its tests and devtools are off here - they run in
# its own repository and its own CI - and it installs nothing.
set(INKCELL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# inkwell first, so this client's pin is the one the whole tree builds: inkcell carries a
# submodule of its own and brings it in only when no target of that name exists yet.
set(INKWELL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/inkwell)
add_subdirectory(third_party/inkcell)

# The application framework over both, and the third cut out of this client: what it keeps is
# the plumbing a second program on this stack would otherwise write again - today the frame
# scheduler under src/ui/nav/controller.c. Added last, so it finds the inkwell and inkcell above
# and builds neither of its own; its nested submodules need not even be checked out.
set(INKSTAND_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/inkstand)
target_link_libraries(meshclient_core PUBLIC inkstand::inkstand)

# inkwell compiles the way this client's own sources do, which matters for exactly two builds.
#
# The loop, the codecs and the resolver used to be files in meshclient_core, so they picked up
# `meshclient_options` - and with it `-fsanitize=address`, `-fsanitize=undefined` and, under
# MESHCLIENT_ENABLE_FUZZERS, `-fsanitize=fuzzer-no-link`. A separate library does not inherit
# any of that. Left alone, moving them out would have quietly turned two things off: the
# sanitizer job would stop checking the code that parses what a server sends, and
# meshclient_fuzz_http and _zip would fuzz a parser with no coverage instrumentation in it -
# a fuzzer with no feedback, which is a random number generator with a corpus.
#
# So inkwell gets the same options target. PRIVATE, because these are how it is compiled here
# and not something a consumer of this build inherits.
#
# inkcell is deliberately not given the same treatment in this commit: its code was never in
# meshclient_core, so this is not a regression there, and instrumenting five thousand lines of
# widget code that has never been through this client's sanitizer job belongs in a change of
# its own rather than riding along with an extraction.
target_link_libraries(inkwell PRIVATE meshclient_options)
# inkstand for the same reason: the frame scheduler was a file in meshclient_core, and the
# sanitizer job was checking it there.
target_link_libraries(inkstand PRIVATE meshclient_options)

add_subdirectory(third_party/nanopb)
target_link_libraries(meshclient_core PUBLIC protobuf-nanopb-static)

# TLS is inkwell's, and so is Mbed TLS.
#
# This client encrypts exactly one connection itself - MQTT to a broker, on behalf of a radio
# that cannot reach one - and fetches over HTTPS for the updater and the firmware catalog.
# Neither is about Meshtastic, so both went down: `inkwell/net/tls.h` is the session and
# `inkwell/net/fetch.h` the request, and the Mbed TLS submodule and its configuration went with
# them. There is nothing to set here; `add_subdirectory(third_party/inkwell)` above brought it
# in, and inkwell links mbedtls PUBLIC so a test in this tree can still stand a server.
#
# Optional on purpose, and still optional here. Without inkwell's submodule `inkwell/net/tls.h`
# refuses every session, `INKWELL_HAVE_TLS` is not defined, and this client builds, links and
# runs - MQTT over TLS is the one thing it cannot do, and it says so. `INKWELL_HAVE_TLS` reaches
# this target through inkwell rather than being minted again here: whether this process can do
# TLS is inkwell's fact about itself, and two names for it would be two things to keep in step.

# Mozilla's roots, written by scripts/gen-ca-roots.py.
#
# Compiled whether or not this build has TLS, on purpose. src/app/app.c hands this table to
# `inkwell_tls_set_roots()` unconditionally - whether Mbed TLS was compiled in is not a question
# the composition root should have to answer, which is the whole reason inkwell/net/tls.c
# compiles to a stub - so the symbols have to exist in both configurations or the no-TLS build
# will not link. This once sat inside a conditional, and that is exactly how it broke. The 126 KB
# is still not carried without TLS: ca_roots.c guards its own payload on INKWELL_HAVE_TLS and
# defines an empty table instead.
target_sources(meshclient_core PRIVATE src/core/generated/ca_roots.c)

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
