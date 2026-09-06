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
