# Host-side development tools. Not part of the client and never shipped in the pak: these
# exist so a UI change can be looked at without a Brick on the desk.
#
# Off by default when cross-compiling, because the aarch64 pak build has no use for them.

add_executable(meshclient_uicap ui_capture/main.c)

target_link_libraries(meshclient_uicap PRIVATE meshclient_core)

target_compile_options(meshclient_uicap PRIVATE ${MESHCLIENT_COMPILE_OPTIONS})
if(MESHCLIENT_LINK_OPTIONS)
    target_link_options(meshclient_uicap PRIVATE ${MESHCLIENT_LINK_OPTIONS})
endif()

add_executable(meshclient_perf perf/main.c)
target_link_libraries(meshclient_perf PRIVATE meshclient_core)
target_compile_options(meshclient_perf PRIVATE ${MESHCLIENT_COMPILE_OPTIONS})
if(MESHCLIENT_LINK_OPTIONS)
    target_link_options(meshclient_perf PRIVATE ${MESHCLIENT_LINK_OPTIONS})
endif()

# The fuzz harnesses, off unless MESHCLIENT_ENABLE_FUZZERS. Each one is a LLVMFuzzerTestOneInput
# and no main(); `-fsanitize=fuzzer` at link time is what provides one, over the coverage
# instrumentation the top-level CMakeLists.txt put on meshclient_core. The seed generator is an
# ordinary program that has its own main, so it links without that flag.
if(MESHCLIENT_ENABLE_FUZZERS)
    add_executable(meshclient_fuzz_seeds fuzz/seeds.c)
    target_link_libraries(meshclient_fuzz_seeds PRIVATE meshclient_core)
    target_compile_options(meshclient_fuzz_seeds PRIVATE ${MESHCLIENT_COMPILE_OPTIONS})
    if(MESHCLIENT_LINK_OPTIONS)
        target_link_options(meshclient_fuzz_seeds PRIVATE ${MESHCLIENT_LINK_OPTIONS})
    endif()

    foreach(fuzz_target IN ITEMS stream_framing session firmware_catalog zip uf2)
        add_executable(meshclient_fuzz_${fuzz_target} fuzz/fuzz_${fuzz_target}.c)
        target_link_libraries(meshclient_fuzz_${fuzz_target} PRIVATE meshclient_core)
        target_compile_options(meshclient_fuzz_${fuzz_target} PRIVATE ${MESHCLIENT_COMPILE_OPTIONS})
        target_link_options(meshclient_fuzz_${fuzz_target} PRIVATE ${MESHCLIENT_LINK_OPTIONS}
                            -fsanitize=fuzzer)
    endforeach()
endif()
