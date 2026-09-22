#include "mesh/core/uf2.h"
#include "framework/mesh_test.h"

/*
 * Which family an architecture means, measured off three real release images rather than read
 * off a table somebody transcribed.
 *
 * The RP2350 is the one worth pinning: it publishes several families - ARM secure, ARM
 * non-secure, RISC-V - and upstream builds the ARM secure one. Guessing wrong there produces a
 * board holding an image its bootloader will not start.
 */
MESH_TEST_CASE(uf2_names_the_family_for_an_architecture, unit) {
    MESH_TEST_FAIL_IF(mesh_uf2_family_for_architecture("nrf52840") != INKWELL_UF2_FAMILY_NRF52840,
                      "the T114's image carries 0xADA52840");
    MESH_TEST_FAIL_IF(mesh_uf2_family_for_architecture("rp2040") != INKWELL_UF2_FAMILY_RP2040,
                      "rp2040-lora's carries 0xE48BFF56");
    MESH_TEST_FAIL_IF(mesh_uf2_family_for_architecture("rp2350") != INKWELL_UF2_FAMILY_RP2350,
                      "and pico2w's carries 0xE48BFF59, the ARM secure one");
    /* 0 is "no UF2 path", not "any family". Every one of these is a board that is flashed some
       other way or not at all. */
    MESH_TEST_FAIL_IF(mesh_uf2_family_for_architecture("esp32-s3") != 0U,
                      "an ESP32-S3 has no UF2 bootloader");
    MESH_TEST_FAIL_IF(mesh_uf2_family_for_architecture("esp32") != 0U, "nor does an ESP32");
    MESH_TEST_FAIL_IF(mesh_uf2_family_for_architecture("nrf52840 ") != 0U,
                      "and the match is exact rather than a prefix");
    MESH_TEST_FAIL_IF(mesh_uf2_family_for_architecture(NULL) != 0U, "NULL is no family");
    record_success(test_name);
}
