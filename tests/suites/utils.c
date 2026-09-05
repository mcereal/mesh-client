#define _POSIX_C_SOURCE 200809L

/* Standalone utilities: SHA-256 and UTF-8 handling. */

#include "framework/mesh_test.h"

#include "mesh/utils/sha256.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Names and message bodies are the two places radio-chosen text reaches the screen, so the
 * UTF-8 helpers they both go through get pinned here: a character is one unit no matter how
 * many bytes it takes, and a malformed byte still advances the cursor.
 */
MESH_TEST_CASE(text_utf8_helpers, unit) {
    /* One four-byte emoji is one character. This is the bug the whole change is about: the
       framebuffer used to walk bytes, so a node named with a single emoji drew four cells. */
    const char *emoji = "\xF0\x9F\x93\xA1";
    if (mesh_text_utf8_length(emoji) != 1U) {
        record_failure(test_name, "a four-byte emoji should be one character");
        return;
    }

    uint32_t codepoint = 0U;
    if (mesh_text_utf8_next(emoji, &codepoint) != 4U || codepoint != 0x1F4E1U) {
        record_failure(test_name, "emoji did not decode to U+1F4E1");
        return;
    }

    struct {
        const char *label;
        const char *text;
        size_t chars;
    } lengths[] = {
        {"ascii", "Trail", 5U},
        {"accented", "Jos\xC3\xA9", 4U},
        {"mixed", "\xF0\x9F\x8C\xB2 Pine", 6U},
        {"empty", "", 0U},
        /* Each malformed byte counts as one character rather than stalling the walk. */
        {"malformed",
         "a\xFF\xFE"
         "b",
         4U},
    };
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
        if (mesh_text_utf8_length(lengths[i].text) != lengths[i].chars) {
            record_failure(test_name, lengths[i].label);
            return;
        }
    }

    /* Truncation lands on a character boundary, never inside a sequence. */
    char line[32];
    snprintf(line, sizeof line, "%s", "\xF0\x9F\x8C\xB2\xF0\x9F\x8F\xA0\xF0\x9F\x9A\x97");
    mesh_text_utf8_truncate(line, 2U);
    if (strcmp(line, "\xF0\x9F\x8C\xB2\xF0\x9F\x8F\xA0") != 0) {
        record_failure(test_name, "truncate split a character");
        return;
    }

    /* And a copy into a buffer too small for the next character stops before it, rather than
       leaving a half sequence behind. */
    char narrow[6];
    mesh_text_sanitise_str("\xF0\x9F\x8C\xB2\xF0\x9F\x8F\xA0", narrow, sizeof narrow);
    if (strcmp(narrow, "\xF0\x9F\x8C\xB2") != 0) {
        record_failure(test_name, "sanitise split a character at the buffer boundary");
        return;
    }

    record_success(test_name);
}

MESH_TEST_CASE(sha256_vectors, unit) {
    /* The published FIPS 180-4 vectors, plus the empty string. */
    static const struct {
        const char *input;
        const char *expected;
    } k_vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for (size_t i = 0; i < sizeof k_vectors / sizeof k_vectors[0]; ++i) {
        struct mesh_sha256 ctx;
        uint8_t digest[MESH_SHA256_DIGEST_LEN];
        char hex[MESH_SHA256_HEX_LEN];
        mesh_sha256_init(&ctx);
        mesh_sha256_update(&ctx, k_vectors[i].input, strlen(k_vectors[i].input));
        mesh_sha256_final(&ctx, digest);
        mesh_sha256_hex(digest, hex, sizeof hex);
        if (strcmp(hex, k_vectors[i].expected) != 0) {
            record_failure(test_name, hex);
            return;
        }
    }

    /* A message longer than one block, fed in awkward pieces: the streaming path and the
       one-shot path must agree, or a download hashed in 4 KB reads would not match. */
    char long_input[1000];
    for (size_t i = 0; i < sizeof long_input; ++i) {
        long_input[i] = (char)('a' + (i % 26U));
    }
    struct mesh_sha256 whole;
    uint8_t whole_digest[MESH_SHA256_DIGEST_LEN];
    mesh_sha256_init(&whole);
    mesh_sha256_update(&whole, long_input, sizeof long_input);
    mesh_sha256_final(&whole, whole_digest);

    struct mesh_sha256 pieces;
    uint8_t pieces_digest[MESH_SHA256_DIGEST_LEN];
    mesh_sha256_init(&pieces);
    for (size_t offset = 0; offset < sizeof long_input;) {
        const size_t chunk = (offset % 7U) + 1U;
        const size_t take = offset + chunk > sizeof long_input ? sizeof long_input - offset : chunk;
        mesh_sha256_update(&pieces, long_input + offset, take);
        offset += take;
    }
    mesh_sha256_final(&pieces, pieces_digest);
    if (memcmp(whole_digest, pieces_digest, sizeof whole_digest) != 0) {
        record_failure(test_name, "streamed and one-shot hashes should agree");
        return;
    }

    /* And the file path, which is what actually verifies a download. */
    char path[] = "/tmp/meshclient_sha256_XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) {
        record_failure(test_name, "could not create a temporary file");
        return;
    }
    if (write(fd, "abc", 3U) != 3) {
        close(fd);
        unlink(path);
        record_failure(test_name, "could not write the temporary file");
        return;
    }
    close(fd);
    uint8_t file_digest[MESH_SHA256_DIGEST_LEN];
    const int hashed = mesh_sha256_file(path, file_digest);
    char file_hex[MESH_SHA256_HEX_LEN];
    mesh_sha256_hex(file_digest, file_hex, sizeof file_hex);
    unlink(path);
    if (hashed != 0 ||
        strcmp(file_hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
        record_failure(test_name, "hashing a file should match the same bytes in memory");
        return;
    }
    if (mesh_sha256_file("/nonexistent/meshclient", file_digest) == 0) {
        record_failure(test_name, "hashing a missing file should fail");
        return;
    }
    record_success(test_name);
}
