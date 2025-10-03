cmake_minimum_required(VERSION 3.18)
project(meshclient VERSION 1.0.0 LANGUAGES C)

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
    src/transport/transport_registry.c
    src/transport/ble/ble_transport.c
    src/transport/ble/bluez_client.c
    src/proto/framing.c
    src/utils/log.c
    src/ui/store.c
    src/ui/controller.c
    src/ui/backends/stub.c
    src/ui/backends/cli.c
    src/ui/backends/minui.c
    src/ui/preferences.c
)

target_include_directories(meshclient_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

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
