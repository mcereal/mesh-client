# The suite is one binary built from many translation units: a framework, shared fixtures under
# support/, and one file per subject area under suites/.
#
# Cases register themselves from constructors (see framework/mesh_test.h), which is why the suite
# files are listed here directly instead of being collected into a static library first: a linker
# is free to drop library members nothing references, and it would take every test inside them
# with it, silently. Add a new suite file to MESHCLIENT_TEST_SUITES and it runs.

set(MESHCLIENT_TEST_FRAMEWORK
    framework/mesh_test.c
)

set(MESHCLIENT_TEST_SUPPORT
    support/data_fixture.c
    support/proto_fixture.c
    support/serial_fixture.c
    support/session_fixture.c
    support/ui_fixture.c
)

set(MESHCLIENT_TEST_SUITES
    suites/app.c
    suites/core_basics.c
    suites/fetch.c
    suites/firmware_catalog.c
    suites/geo.c
    suites/i18n.c
    suites/json.c
    suites/map.c
    suites/message.c
    suites/radio_settings.c
    suites/session.c
    suites/store_forward.c
    suites/transport_ble_data.c
    suites/transport_ble_link.c
    suites/transport_ble_pairing.c
    suites/transport_serial.c
    suites/ui_actions.c
    suites/ui_anim.c
    suites/ui_capture.c
    suites/ui_chrome.c
    suites/ui_delivery.c
    suites/ui_help.c
    suites/ui_history.c
    suites/ui_icon.c
    suites/ui_input.c
    suites/ui_layout.c
    suites/ui_nav_messaging.c
    suites/ui_nav_nodes.c
    suites/ui_nav_settings.c
    suites/ui_preferences.c
    suites/ui_route.c
    suites/ui_settings.c
    suites/ui_status.c
    suites/ui_store.c
    suites/ui_text.c
    suites/ui_theme.c
    suites/updater.c
    suites/utils.c
    suites/waypoints.c
)

add_executable(meshclient_core_tests
    ${MESHCLIENT_TEST_FRAMEWORK}
    ${MESHCLIENT_TEST_SUPPORT}
    ${MESHCLIENT_TEST_SUITES}
)

# Lets a suite say #include "framework/mesh_test.h" and #include "support/proto_fixture.h".
target_include_directories(meshclient_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The captured documents in data/ are read from the *source* tree, not copied into the build
# one: a fixture refreshed on disk is the one the next run reads, and some of them (a zip's
# central directory, once phase 2 arrives) are not text and could never have been literals.
target_compile_definitions(meshclient_core_tests
    PRIVATE MESH_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

target_link_libraries(meshclient_core_tests PRIVATE meshclient_core)

target_compile_options(meshclient_core_tests PRIVATE ${MESHCLIENT_COMPILE_OPTIONS})
if(MESHCLIENT_LINK_OPTIONS)
    target_link_options(meshclient_core_tests PRIVATE ${MESHCLIENT_LINK_OPTIONS})
endif()

add_test(NAME meshclient_core_unit COMMAND meshclient_core_tests --category unit)
set_tests_properties(meshclient_core_unit PROPERTIES LABELS "unit")

# The catalog is only the single source of the client's text for as long as nobody puts an
# English sentence back into a renderer, and a string literal is not something the compiler has
# an opinion about. check-strings.py is that opinion; docs/i18n.md explains what it is for.
add_test(NAME meshclient_hardcoded_strings
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check-strings.py)
set_tests_properties(meshclient_hardcoded_strings PROPERTIES LABELS "unit")

# scripts/frames.py hand-rolls GIF's variable-width LZW, and the one thing that is easy to get
# subtly wrong there - when the code width grows - produces a file that still opens and shows
# garbage. `selftest` round-trips the compressor through an independently written decoder, and
# checks the bound on the colour histogram: median cut scales with the number of distinct
# colours, so an unbounded one is not a slow path but a hang on any screen that is a photo.
add_test(NAME meshclient_frames_codec
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/frames.py selftest)
set_tests_properties(meshclient_frames_codec PROPERTIES LABELS "unit")

if(DBUS_FOUND)
    find_program(DBUS_RUN_SESSION dbus-run-session)
    if(DBUS_RUN_SESSION)
        add_executable(meshclient_bluez_bus_tests bluez_bus.c)
        target_include_directories(meshclient_bluez_bus_tests PRIVATE ${DBUS_INCLUDE_DIRS})
        target_link_libraries(meshclient_bluez_bus_tests PRIVATE meshclient_core)
        target_compile_options(meshclient_bluez_bus_tests PRIVATE ${MESHCLIENT_COMPILE_OPTIONS})
        if(MESHCLIENT_LINK_OPTIONS)
            target_link_options(meshclient_bluez_bus_tests PRIVATE ${MESHCLIENT_LINK_OPTIONS})
        endif()
        add_test(NAME meshclient_bluez_bus COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:meshclient_bluez_bus_tests>)
        set_tests_properties(meshclient_bluez_bus PROPERTIES LABELS integration TIMEOUT 15)
    endif()
endif()
