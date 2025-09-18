cmake_minimum_required(VERSION 3.18)
project(meshclient VERSION 0.1.0 LANGUAGES C)

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
    src/utils/log.c
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

target_compile_options(meshclient PRIVATE ${MESHCLIENT_COMPILE_OPTIONS})
if(MESHCLIENT_LINK_OPTIONS)
    target_link_options(meshclient PRIVATE ${MESHCLIENT_LINK_OPTIONS})
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS meshclient RUNTIME DESTINATION bin)
