/*
 * A release zip's back end, fed whatever arrives.
 *
 * This reader's whole job is length arithmetic over bytes somebody else sent: a window scanned
 * backwards for a signature, a record whose fields say where a directory is, a directory of
 * variable-length entries walked by adding three 16-bit numbers, and a local header that places
 * half a megabyte by adding two more. Every one of those is a place where a hostile - or merely
 * corrupt - reply gets to choose an offset, which is exactly the shape `make fuzz` exists for.
 *
 * It is also the one parser here whose *output* is an instruction to fetch: an offset that came
 * back wrong is a range request for somebody else's megabyte, so the contracts below are about
 * the answers as much as about the reads.
 *
 * Build with scripts/fuzz.sh; see docs/testing.md.
 */

#include "mesh/utils/zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fuzz_broke(const char *what) {
    fprintf(stderr, "zip contract broken: %s\n", what);
    fflush(stderr);
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4U) {
        return 0;
    }
    /*
     * The first two bytes choose where in a file the window is pretending to be, so the corpus
     * explores the arithmetic that relates the record's absolute offsets to the window's own -
     * which is where an underflow would live - rather than always reading a whole file.
     */
    const uint64_t window_offset = (uint64_t)data[0] | ((uint64_t)data[1] << 8);
    const uint8_t *const window = data + 2U;
    const size_t len = size - 2U;

    struct mesh_zip_end end;
    if (mesh_zip_find_end(window, len, window_offset, &end)) {
        /* Whatever it says, the directory it points at has to end at or before the record - a
           reader that let those cross would be asking the CDN for bytes after the file. */
        if (end.central_offset + (uint64_t)end.central_size > window_offset + (uint64_t)len) {
            fuzz_broke("a directory that ends past the window it was found in");
        }
        const uint8_t *const central = mesh_zip_central_slice(&end, window, len, window_offset);
        if (central != NULL) {
            /* A slice that came back must be wholly inside the buffer we handed over. Reading
               both ends is what makes the sanitizer check the claim rather than the sums. */
            if (central < window || central + end.central_size > window + len) {
                fuzz_broke("a slice outside the window");
            }
            volatile uint8_t touch = central[0];
            touch = central[end.central_size > 0U ? end.central_size - 1U : 0U];
            (void)touch;

            struct mesh_zip_entry entry;
            /* Two names: one that the input can be made to contain, and one that no directory
               walk should ever match, so both sides of the search are explored. */
            static const char *const k_wanted[] = {"firmware.uf2", ""};
            for (size_t i = 0; i < sizeof k_wanted / sizeof k_wanted[0]; ++i) {
                if (!mesh_zip_find_member(central, end.central_size, end.entries, k_wanted[i],
                                          &entry)) {
                    continue;
                }
                if (memchr(entry.name, '\0', sizeof entry.name) == NULL) {
                    fuzz_broke("an unterminated member name");
                }
                if (entry.name[0] == '\0') {
                    fuzz_broke("a member with no name should not have matched");
                }
                uint64_t start = 0U;
                if (mesh_zip_local_data_start(window, len, &entry, &start)) {
                    if (start < entry.local_header_offset + MESH_ZIP_LOCAL_HEADER_SIZE) {
                        fuzz_broke("data placed before the header that placed it");
                    }
                }
            }
        }
    }

    /* And the local header on its own, over the raw input, so it is exercised without having
       to reach it through a directory the fuzzer has to invent first. */
    struct mesh_zip_entry loose;
    memset(&loose, 0, sizeof loose);
    loose.local_header_offset = window_offset;
    uint64_t start = 0U;
    (void)mesh_zip_local_data_start(window, len, &loose, &start);
    return 0;
}
