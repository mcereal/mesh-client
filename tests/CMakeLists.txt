add_executable(meshclient_core_tests
    test_main.c
)

target_link_libraries(meshclient_core_tests PRIVATE meshclient_core)

target_compile_options(meshclient_core_tests PRIVATE ${MESHCLIENT_COMPILE_OPTIONS})
if(MESHCLIENT_LINK_OPTIONS)
    target_link_options(meshclient_core_tests PRIVATE ${MESHCLIENT_LINK_OPTIONS})
endif()

add_test(NAME meshclient_core_unit COMMAND meshclient_core_tests --category unit)
set_tests_properties(meshclient_core_unit PROPERTIES LABELS "unit")
