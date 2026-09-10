/*
 * Reading a release zip from its back end, against two real release zips.
 *
 * The fixtures are the last 64 KB of `firmware-nrf52840-2.7.26.54e0d8d.zip` and of
 * `firmware-nrf52840-2.8.0.47db0e3.zip`, fetched with exactly the range request the client
 * makes. **Two of them and not one**, because those two releases differ in both of the ways a
 * zip can quietly break this reader - a flat member path against a `nrf52840/` prefixed one,
 * and a local header that agrees with the central directory against one whose CRC and both
 * sizes are 0 - and a suite that holds only the older one passes while the code is wrong.
 *
 * The windows do not start at the beginning of the file, so every case has to hand the reader
 * the offset the window was fetched from. That is not test scaffolding: it is the shape of the
 * download, and a reader that could only work on a whole file would be a reader this client
 * could not use.
 */

#include "framework/mesh_test.h"
#include "support/data_fixture.h"

#include "mesh/utils/zip.h"

#include <stdlib.h>
#include <string.h>

/* The two zips' real lengths, from the `Content-Range` of a one-byte probe against the CDN. */
#define ZIP_SIZE_2_7_26 46259773ULL
#define ZIP_SIZE_2_8_0 58592416ULL

struct zip_fixture {
    uint8_t *window;
    size_t len;
    uint64_t offset;
};

static bool zip_open(const char *name, uint64_t zip_size, struct zip_fixture *out) {
    size_t len = 0U;
    char *const bytes = mesh_test_data_read(name, &len);
    if (bytes == NULL || len == 0U || (uint64_t)len > zip_size) {
        free(bytes);
        return false;
    }
    out->window = (uint8_t *)bytes;
    out->len = len;
    out->offset = zip_size - (uint64_t)len;
    return true;
}

static void zip_close(struct zip_fixture *fixture) { free(fixture->window); }

/*
 * The whole three-step read, on the release the client was proven against by hand.
 *
 * Every number here was measured off the served file rather than computed by this code: the
 * directory is 139 entries and 16,316 bytes, and the T114's UF2 is 517,956 compressed bytes
 * that inflate to 1,467,392 with CRC 3175933648.
 */
MESH_TEST_CASE(zip_reads_a_release_tail, unit) {
    struct zip_fixture zip;
    MESH_TEST_FAIL_IF(!zip_open("zip_tail_nrf52840_2.7.26.bin", ZIP_SIZE_2_7_26, &zip),
                      "the 2.7.26 tail window should be readable");

    struct mesh_zip_end end;
    const bool found = mesh_zip_find_end(zip.window, zip.len, zip.offset, &end);
    MESH_TEST_FAIL_IF_CLEANUP(!found, zip_close(&zip),
                              "the end-of-central-directory record is in the window");
    MESH_TEST_FAIL_IF_CLEANUP(end.entries != 139U, zip_close(&zip),
                              "2.7.26's nrf52840 zip holds 139 members");
    MESH_TEST_FAIL_IF_CLEANUP(end.central_size != 16316U, zip_close(&zip),
                              "and a 16,316-byte directory");

    /* The point of a 64 KB window: both these directories are already in it, so the download
       is two round trips rather than three. */
    const uint8_t *const central = mesh_zip_central_slice(&end, zip.window, zip.len, zip.offset);
    MESH_TEST_FAIL_IF_CLEANUP(central == NULL, zip_close(&zip),
                              "the directory fits inside the tail window");

    struct mesh_zip_entry entry;
    const bool member =
        mesh_zip_find_member(central, end.central_size, end.entries,
                             "firmware-heltec-mesh-node-t114-2.7.26.54e0d8d.uf2", &entry);
    MESH_TEST_FAIL_IF_CLEANUP(!member, zip_close(&zip),
                              "the T114's image should be in the directory");
    MESH_TEST_FAIL_IF_CLEANUP(entry.compressed_size != 517956U ||
                                  entry.uncompressed_size != 1467392U,
                              zip_close(&zip), "both sizes come off the central directory");
    MESH_TEST_FAIL_IF_CLEANUP(entry.crc32 != 3175933648U, zip_close(&zip),
                              "and so does the CRC the inflate is checked against");
    MESH_TEST_FAIL_IF_CLEANUP(entry.method != MESH_ZIP_METHOD_DEFLATE, zip_close(&zip),
                              "release members are deflated");
    MESH_TEST_FAIL_IF_CLEANUP(entry.local_header_offset != 33671990ULL, zip_close(&zip),
                              "the local header is where the served file has it");
    /* Flat at this release. The next case is the same file one release later. */
    MESH_TEST_FAIL_IF_CLEANUP(strchr(entry.name, '/') != NULL, zip_close(&zip),
                              "2.7.26 stores its members at the root of the zip");
    zip_close(&zip);
    record_success(test_name);
}

/*
 * The same read one release later, where the member has moved and the local header has emptied.
 *
 * Both changes are silent: a full-path match finds nothing and reports "this release does not
 * build for your board", and a reader taking its sizes from the local header asks the CDN for
 * zero bytes and checks them against a CRC of 0, which passes.
 */
MESH_TEST_CASE(zip_reads_a_prefixed_member, unit) {
    struct zip_fixture zip;
    MESH_TEST_FAIL_IF(!zip_open("zip_tail_nrf52840_2.8.0.bin", ZIP_SIZE_2_8_0, &zip),
                      "the 2.8.0 tail window should be readable");

    struct mesh_zip_end end;
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_zip_find_end(zip.window, zip.len, zip.offset, &end),
                              zip_close(&zip), "2.8.0's record is in the window too");
    MESH_TEST_FAIL_IF_CLEANUP(end.entries != 171U || end.central_size != 21358U, zip_close(&zip),
                              "171 members and a 21,358-byte directory");

    const uint8_t *const central = mesh_zip_central_slice(&end, zip.window, zip.len, zip.offset);
    MESH_TEST_FAIL_IF_CLEANUP(central == NULL, zip_close(&zip),
                              "a bigger directory still fits the window");

    struct mesh_zip_entry entry;
    const bool member =
        mesh_zip_find_member(central, end.central_size, end.entries,
                             "firmware-heltec-mesh-node-t114-2.8.0.47db0e3.uf2", &entry);
    MESH_TEST_FAIL_IF_CLEANUP(!member, zip_close(&zip),
                              "the basename should find it under its new prefix");
    MESH_TEST_FAIL_IF_CLEANUP(
        strcmp(entry.name, "nrf52840/firmware-heltec-mesh-node-t114-2.8.0.47db0e3.uf2") != 0,
        zip_close(&zip), "and the stored path is the prefixed one");
    MESH_TEST_FAIL_IF_CLEANUP((entry.flags & MESH_ZIP_FLAG_DATA_DESCRIPTOR) == 0U, zip_close(&zip),
                              "2.8.0's members defer their sizes to a data descriptor");
    MESH_TEST_FAIL_IF_CLEANUP(entry.compressed_size != 561092U ||
                                  entry.uncompressed_size != 1491968U || entry.crc32 != 2320343302U,
                              zip_close(&zip),
                              "which is why all three of those come from the directory");
    zip_close(&zip);
    record_success(test_name);
}

/*
 * A directory marker is an entry with an empty basename, and must never match.
 *
 * 2.8.0's zip opens with `nrf52840/` - stored, zero-length, no flags - and it is the entry a
 * basename walk reaches first. A matcher that let an empty basename through would hand the
 * download a member whose bytes are a directory.
 */
MESH_TEST_CASE(zip_never_matches_a_directory_marker, unit) {
    struct zip_fixture zip;
    MESH_TEST_FAIL_IF(!zip_open("zip_tail_nrf52840_2.8.0.bin", ZIP_SIZE_2_8_0, &zip),
                      "the 2.8.0 tail window should be readable");
    struct mesh_zip_end end;
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_zip_find_end(zip.window, zip.len, zip.offset, &end),
                              zip_close(&zip), "the record should be found");
    const uint8_t *const central = mesh_zip_central_slice(&end, zip.window, zip.len, zip.offset);
    MESH_TEST_FAIL_IF_CLEANUP(central == NULL, zip_close(&zip),
                              "the directory should be in the window");

    struct mesh_zip_entry entry;
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_zip_find_member(central, end.central_size, end.entries, "", &entry), zip_close(&zip),
        "an empty basename matches nothing");
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_zip_find_member(central, end.central_size, end.entries, "nrf52840/", &entry),
        zip_close(&zip), "and neither does the directory's own stored name");
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_zip_find_member(central, end.central_size, end.entries,
                             "firmware-heltec-mesh-node-t114-2.7.26.54e0d8d.uf2", &entry),
        zip_close(&zip), "nor does the previous release's file name");
    zip_close(&zip);
    record_success(test_name);
}

/*
 * The local header answers one question and the answer differs from the directory's.
 *
 * At 2.7.26 the T114's UF2 has a 49-byte name and a 28-byte extra field in its local header,
 * against a name of the same length and a *different* extra field in the directory - the two
 * places carry different tags. That difference is the whole reason for the third range read,
 * so it is worth pinning rather than assuming.
 */
MESH_TEST_CASE(zip_places_the_data_from_the_local_header, unit) {
    struct mesh_zip_entry entry;
    memset(&entry, 0, sizeof entry);
    entry.local_header_offset = 33671990ULL;

    /* The 30 fixed bytes as served, name length 49 and extra length 28. */
    uint8_t header[MESH_ZIP_LOCAL_HEADER_SIZE];
    memset(header, 0, sizeof header);
    header[0] = 0x50U;
    header[1] = 0x4BU;
    header[2] = 0x03U;
    header[3] = 0x04U;
    header[26] = 49U;
    header[28] = 28U;

    uint64_t start = 0U;
    MESH_TEST_FAIL_IF(!mesh_zip_local_data_start(header, sizeof header, &entry, &start),
                      "a local header should place its data");
    MESH_TEST_FAIL_IF(start != 33671990ULL + 30ULL + 49ULL + 28ULL,
                      "the data begins after the header, the name and the extra field");

    header[0] = 0x00U;
    MESH_TEST_FAIL_IF(mesh_zip_local_data_start(header, sizeof header, &entry, &start),
                      "and a header with no signature is refused");
    MESH_TEST_FAIL_IF(mesh_zip_local_data_start(header, 12U, &entry, &start),
                      "as is one that arrived short");
    record_success(test_name);
}

/*
 * A directory that does not fit the window is a question, not a failure.
 *
 * Both real zips fit theirs with room to spare, so the case that matters is the one that does
 * not - and the honest answer is "range-read the directory itself", which is what a NULL slice
 * tells the caller to do. Simulated by handing the reader a window that starts later than it
 * really did, which is exactly what a smaller tail request would have produced.
 */
MESH_TEST_CASE(zip_says_when_the_directory_is_outside_the_window, unit) {
    struct zip_fixture zip;
    MESH_TEST_FAIL_IF(!zip_open("zip_tail_nrf52840_2.7.26.bin", ZIP_SIZE_2_7_26, &zip),
                      "the 2.7.26 tail window should be readable");
    struct mesh_zip_end end;
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_zip_find_end(zip.window, zip.len, zip.offset, &end),
                              zip_close(&zip), "the record should be found");

    /* The last 8 KB of the same window: the record is still in it, the 16 KB directory is not. */
    const size_t small = 8192U;
    const uint8_t *const shifted = zip.window + (zip.len - small);
    const uint64_t shifted_offset = zip.offset + (uint64_t)(zip.len - small);

    struct mesh_zip_end from_small;
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_zip_find_end(shifted, small, shifted_offset, &from_small),
                              zip_close(&zip), "a smaller window still finds the record");
    MESH_TEST_FAIL_IF_CLEANUP(from_small.central_offset != end.central_offset, zip_close(&zip),
                              "and reports the same directory");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_zip_central_slice(&from_small, shifted, small, shifted_offset) !=
                                  NULL,
                              zip_close(&zip), "but cannot hand back a directory it does not hold");
    zip_close(&zip);
    record_success(test_name);
}

/*
 * The signature turns up inside the payload, and the comment length is what tells them apart.
 *
 * Not hypothetical: these members are compressed firmware, so four given bytes appear about
 * where chance says they should. The scan runs backwards and takes the first candidate whose
 * comment length accounts for exactly the bytes after it, and this checks that the record it
 * settled on is the real one by reading the directory it points at.
 */
MESH_TEST_CASE(zip_finds_the_record_and_not_a_coincidence, unit) {
    struct zip_fixture zip;
    MESH_TEST_FAIL_IF(!zip_open("zip_tail_nrf52840_2.7.26.bin", ZIP_SIZE_2_7_26, &zip),
                      "the 2.7.26 tail window should be readable");
    struct mesh_zip_end end;
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_zip_find_end(zip.window, zip.len, zip.offset, &end),
                              zip_close(&zip), "the record should be found");

    const uint8_t *const central = mesh_zip_central_slice(&end, zip.window, zip.len, zip.offset);
    MESH_TEST_FAIL_IF_CLEANUP(central == NULL, zip_close(&zip),
                              "the directory should be in the window");
    /* The first four bytes of the directory it named are a central header signature. A record
       found by coincidence would point somewhere these are not. */
    MESH_TEST_FAIL_IF_CLEANUP(
        central[0] != 0x50U || central[1] != 0x4BU || central[2] != 0x01U || central[3] != 0x02U,
        zip_close(&zip), "the offset it reported is a real central directory");

    /* A truncated window - one holding less than a whole record - has no answer at all. */
    struct mesh_zip_end nothing;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_zip_find_end(zip.window, 8U, zip.offset, &nothing),
                              zip_close(&zip),
                              "and a window too short to hold a record finds none");
    zip_close(&zip);
    record_success(test_name);
}
