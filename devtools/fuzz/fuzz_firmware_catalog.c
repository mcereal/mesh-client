/*
 * The two documents that decide which firmware a radio gets offered, fed whatever arrives.
 *
 * This is the third door bytes we did not write come in through, and unlike the other two it is
 * not a radio on the other end: it is HTTPS to a service, parsed by a hand-written reader over
 * a buffer whose length is the only thing standing between it and the rest of the heap. Length
 * arithmetic over attacker-shaped input is what `make fuzz` is for.
 *
 * Memory safety is the first oracle and the sanitizers provide it. Three contracts are checked
 * on top, because a parser can be memory-safe and still be wrong in a way that matters here:
 *
 *   1. Every string that comes back is NUL-terminated inside its own field. A reader that
 *      filled a buffer without terminating it would be found by whatever printed it next,
 *      which is a long way from here.
 *   2. `count` never exceeds `found`, and neither exceeds what the array can hold. The
 *      ambiguous-hw_model answer is built on that pair, and a `count` past the array is a
 *      write past it one revision later.
 *   3. Ordering is a total order: comparing anything with itself is 0, and swapping the
 *      arguments negates the answer. A comparator that is not is how a client offers an update
 *      to the version it is already running.
 *
 * Build with scripts/fuzz.sh; see docs/testing.md.
 */

#include "mesh/core/firmware_catalog.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fuzz_broke(const char *what) {
    fprintf(stderr, "firmware catalog contract broken: %s\n", what);
    fflush(stderr);
    abort();
}

/* A field is well formed when a terminator lies inside it - which is also the read that makes
   the sanitizer look at every byte the parser claimed to write. */
static void check_terminated(const char *field, size_t size, const char *what) {
    if (memchr(field, '\0', size) == NULL) {
        fuzz_broke(what);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The parsers take a length, but they are also given documents that are C strings, so the
       copy is NUL-terminated: an off-by-one that walks past `len` should still be caught by the
       sanitizer at the end of the allocation rather than run into whatever follows. */
    char *document = malloc(size + 1U);
    if (document == NULL) {
        return 0;
    }
    if (size > 0U) {
        memcpy(document, data, size);
    }
    document[size] = '\0';

    /* The model number comes off the first bytes of the input, so the corpus explores both the
       found and the not-found sides rather than always asking about a board nothing claims. */
    const uint32_t hw_model = size >= 2U ? (uint32_t)data[0] | ((uint32_t)data[1] << 8) : 0U;

    struct mesh_firmware_boards boards;
    if (mesh_firmware_boards_parse(document, size, hw_model, &boards)) {
        if (boards.count > MESH_FIRMWARE_BOARDS_MAX || boards.count > boards.found) {
            fuzz_broke("count should be the kept subset of found");
        }
        for (uint8_t i = 0; i < boards.count; ++i) {
            const struct mesh_firmware_board *const board = &boards.entries[i];
            check_terminated(board->target, sizeof board->target, "an unterminated target");
            check_terminated(board->name, sizeof board->name, "an unterminated board name");
            check_terminated(board->architecture, sizeof board->architecture,
                             "an unterminated architecture");
            if (board->hw_model != hw_model) {
                fuzz_broke("a board that does not claim the model that was asked for");
            }
            if (board->target[0] == '\0') {
                fuzz_broke("a board with no build target should not have been kept");
            }
            if (board->path != mesh_firmware_path_for_architecture(board->architecture)) {
                fuzz_broke("the path should be a function of the architecture and nothing else");
            }
        }
    }

    struct mesh_firmware_release release;
    for (int channel = 0; channel < MESH_FIRMWARE_CHANNEL_COUNT; ++channel) {
        if (!mesh_firmware_release_parse(document, size, (enum mesh_firmware_channel)channel,
                                         &release)) {
            continue;
        }
        check_terminated(release.version, sizeof release.version, "an unterminated version");
        check_terminated(release.manifest_url, sizeof release.manifest_url,
                         "an unterminated manifest url");
        if (release.version[0] == '\0') {
            fuzz_broke("a release with no version should not have parsed");
        }
        /* The comparator, over a string that came out of the document rather than one we
           chose - which is the only way it ever sees the shapes this fuzzer invents. */
        if (mesh_firmware_version_compare(release.version, release.version) != 0) {
            fuzz_broke("a version should equal itself");
        }
        static const char *const k_against[] = {"", "0", "2.7.26.54e0d8d", "99.99.99"};
        for (size_t i = 0; i < sizeof k_against / sizeof k_against[0]; ++i) {
            const int forward = mesh_firmware_version_compare(release.version, k_against[i]);
            const int backward = mesh_firmware_version_compare(k_against[i], release.version);
            if ((forward < 0) != (backward > 0) || (forward == 0) != (backward == 0)) {
                fuzz_broke("comparing the other way round should negate the answer");
            }
        }
    }

    free(document);
    return 0;
}
