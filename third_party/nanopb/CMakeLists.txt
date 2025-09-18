add_library(nanopb_stub STATIC
    src/pb_stub.c
)

target_include_directories(nanopb_stub
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)
