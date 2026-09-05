cmake_minimum_required(VERSION 3.18)
project(meshclient VERSION 1.19.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MESHCLIENT_ENABLE_ASAN "Enable AddressSanitizer for debugging" OFF)
option(MESHCLIENT_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for debugging" OFF)

list(APPEND MESHCLIENT_COMPILE_OPTIONS
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
    list(APPEND MESHCLIENT_COMPILE_OPTIONS -fsanitize=address)
    list(APPEND MESHCLIENT_LINK_OPTIONS -fsanitize=address)
endif()

if(MESHCLIENT_ENABLE_UBSAN)
    list(APPEND MESHCLIENT_COMPILE_OPTIONS -fsanitize=undefined)
    list(APPEND MESHCLIENT_LINK_OPTIONS -fsanitize=undefined)
endif()

add_library(meshclient_core
    src/core/app.c
    src/core/config.c
    src/core/event_loop.c
    src/core/message.c
    src/core/radio_settings.c
    src/core/session.c
    src/core/signals.c
    src/core/updater.c
    src/core/version.c
    src/transport/transport_registry.c
    src/transport/ble/ble_transport.c
    src/transport/ble/bluez_client.c
    src/transport/serial/serial_transport.c
    src/transport/serial/serial_usb.c
    src/proto/framing.c
    src/proto/stream_framing.c
    src/utils/log.c
    src/utils/sha256.c
    src/utils/text.c
    src/ui/store.c
    src/ui/controller.c
    src/ui/input.c
    src/ui/nav.c
    src/ui/settings.c
    src/ui/node_detail.c
    src/ui/backends/stub.c
    src/ui/backends/cli.c
    src/ui/backends/minui.c
    src/ui/backends/fb.c
    src/ui/font5x7.c
    src/ui/emoji.c
    src/ui/emoji_glyphs.c
    src/ui/preferences.c
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

target_compile_options(meshclient_core PRIVATE ${MESHCLIENT_COMPILE_OPTIONS})
if(MESHCLIENT_LINK_OPTIONS)
    target_link_options(meshclient_core PRIVATE ${MESHCLIENT_LINK_OPTIONS})
endif()

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


target_link_libraries(meshclient_core
    PUBLIC
        Threads::Threads
)

add_executable(meshclient src/main.c)

target_link_libraries(meshclient PRIVATE meshclient_core)

set(nanopb_BUILD_GENERATOR OFF CACHE BOOL "Disable nanopb generator build" FORCE)
set(nanopb_BUILD_RUNTIME ON CACHE BOOL "Enable nanopb runtime" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Use static nanopb" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build static nanopb" FORCE)

add_subdirectory(third_party/nanopb)
target_link_libraries(meshclient_core PUBLIC protobuf-nanopb-static)

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
    device_ui
    xmodem
    atak
    admin
    connection_status
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
        COMMAND ${NANOPB_GENERATOR_COMMAND}
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

target_compile_options(meshclient PRIVATE ${MESHCLIENT_COMPILE_OPTIONS})
if(MESHCLIENT_LINK_OPTIONS)
    target_link_options(meshclient PRIVATE ${MESHCLIENT_LINK_OPTIONS})
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS meshclient RUNTIME DESTINATION bin)
