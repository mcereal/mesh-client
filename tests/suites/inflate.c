/*
 * The inflate and the CRC32 a firmware download is checked with.
 *
 * The first case is a real member - the T114's `.mt.json`, deflated, exactly as the CDN serves
 * it out of the 2.7.26 release zip - and the CRC it must come out to is the one that zip's
 * central directory carries for it. The rest are the refusals, which are what the download
 * turns into its ERROR_INFLATE row, and the one answer that is not a refusal: a stored block.
 */

#include "framework/mesh_test.h"
#include "support/data_fixture.h"

#include "mesh/utils/inflate.h"

#include <stdlib.h>
#include <string.h>

/* Where the deflated payload starts in the fixture - past the 30-byte local header, the 53-byte
   name and the 28-byte extra field - and what the central directory says it inflates to. */
#define MEMBER_DATA_OFFSET 111U
#define MEMBER_INFLATED 1157U
#define MEMBER_CRC 0xE9FF7C29U

MESH_TEST_CASE(inflate_reads_a_real_zip_member, unit) {
    size_t len = 0U;
    char *const fixture = mesh_test_data_read("zip_member_t114_mt_json_2.7.26.bin", &len);
    MESH_TEST_FAIL_IF(fixture == NULL || len <= MEMBER_DATA_OFFSET, "the member fixture loads");
    const uint8_t *const deflated = (const uint8_t *)fixture + MEMBER_DATA_OFFSET;
    const size_t deflated_len = len - MEMBER_DATA_OFFSET;

    uint8_t out[MEMBER_INFLATED];
    size_t produced = 0U;
    const enum mesh_inflate_result result =
        mesh_inflate(deflated, deflated_len, out, sizeof out, &produced);
    uint32_t crc = 0U;
    const bool hashed = mesh_crc32(out, produced, &crc);

    /* One byte short of room is not corruption - every byte written is right, there is just one
       more of them - and the download has to be able to tell the two apart in a log. */
    uint8_t short_out[MEMBER_INFLATED - 1U];
    size_t short_produced = 0U;
    const enum mesh_inflate_result too_long =
        mesh_inflate(deflated, deflated_len, short_out, sizeof short_out, &short_produced);

    /* And a member cut off partway is, because the stream never reaches its final block. */
    size_t cut_produced = 0U;
    const enum mesh_inflate_result cut =
        mesh_inflate(deflated, deflated_len / 2U, out, sizeof out, &cut_produced);
    free(fixture);

    MESH_TEST_FAIL_IF(result != MESH_INFLATE_OK, "the member inflates");
    MESH_TEST_FAIL_IF(produced != MEMBER_INFLATED, "to the length the directory promised");
    MESH_TEST_FAIL_IF(!hashed || crc != MEMBER_CRC, "and the CRC the directory carried");
    MESH_TEST_FAIL_IF(out[0] != '{', "and what falls out is the manifest's JSON");
    MESH_TEST_FAIL_IF(too_long != MESH_INFLATE_TOO_LONG,
                      "a stream longer than the room is TOO_LONG");
    MESH_TEST_FAIL_IF(short_produced != sizeof short_out, "having filled all of the room it had");
    MESH_TEST_FAIL_IF(cut != MESH_INFLATE_CORRUPT, "a truncated member is CORRUPT");
    record_success(test_name);
}

MESH_TEST_CASE(inflate_takes_a_stored_block_and_refuses_a_reserved_one, unit) {
    /* BFINAL, BTYPE 00, then LEN 3 and its complement, then the three bytes as they are. */
    static const uint8_t k_stored[] = {0x01U, 0x03U, 0x00U, 0xFCU, 0xFFU, 'a', 'b', 'c'};
    uint8_t out[8];
    size_t produced = 0U;
    MESH_TEST_FAIL_IF(mesh_inflate(k_stored, sizeof k_stored, out, sizeof out, &produced) !=
                          MESH_INFLATE_OK,
                      "a stored block is a deflate stream like any other");
    MESH_TEST_FAIL_IF(produced != 3U || memcmp(out, "abc", 3U) != 0, "and yields its bytes");

    /* BFINAL with BTYPE 11, which RFC 1951 reserves: an error, not a block to skip. */
    static const uint8_t k_reserved[] = {0x07U, 0x00U, 0x00U, 0x00U};
    MESH_TEST_FAIL_IF(mesh_inflate(k_reserved, sizeof k_reserved, out, sizeof out, &produced) !=
                          MESH_INFLATE_CORRUPT,
                      "a reserved block type is CORRUPT");
    MESH_TEST_FAIL_IF(mesh_inflate(NULL, 4U, out, sizeof out, &produced) != MESH_INFLATE_CORRUPT,
                      "and so is no input at all");
    record_success(test_name);
}

MESH_TEST_CASE(crc32_matches_the_standard_check_value, unit) {
    /* The check value every CRC-32/ISO-HDLC implementation publishes for "123456789". */
    uint32_t crc = 1U;
    MESH_TEST_FAIL_IF(!mesh_crc32((const uint8_t *)"123456789", 9U, &crc) || crc != 0xCBF43926U,
                      "CRC32 of \"123456789\" is 0xCBF43926");
    MESH_TEST_FAIL_IF(!mesh_crc32(NULL, 0U, &crc) || crc != 0U, "and of nothing is 0");
    MESH_TEST_FAIL_IF(mesh_crc32(NULL, 1U, &crc), "a length with no bytes is refused");
    record_success(test_name);
}
