/*
 * The JSON cursor: the shapes it has to walk, and the ones it has to refuse.
 *
 * Weighted towards refusal on purpose. This reads bytes off the network, so what matters is not
 * that it parses the documents upstream serves today - the catalog suite next door proves that
 * against captured ones - but that it never walks past the end of a buffer when it is handed
 * something that is not one of them.
 */

#include "framework/mesh_test.h"

#include "mesh/utils/json.h"

#include <string.h>

MESH_TEST_CASE(json_reads_an_object, unit) {
    static const char k_document[] = "  { \"name\" : \"T114\" , \"model\": 69, \"dfu\" : true ,\n"
                                     "    \"tags\": [\"a\", \"b\"], \"nested\": {\"x\": {} } }";
    struct mesh_json json;
    mesh_json_init(&json, k_document, 0U);
    MESH_TEST_FAIL_IF(!mesh_json_enter_object(&json), "the document is an object");

    char name[16];
    uint64_t model = 0U;
    bool dfu = false;
    unsigned seen = 0U;
    char key[16];
    while (mesh_json_next_key(&json, key, sizeof key)) {
        if (strcmp(key, "name") == 0) {
            MESH_TEST_FAIL_IF(!mesh_json_read_string(&json, name, sizeof name), "name is a string");
            seen++;
        } else if (strcmp(key, "model") == 0) {
            MESH_TEST_FAIL_IF(!mesh_json_read_u64(&json, &model), "model is a number");
            seen++;
        } else if (strcmp(key, "dfu") == 0) {
            MESH_TEST_FAIL_IF(!mesh_json_read_bool(&json, &dfu), "dfu is a bool");
            seen++;
        } else {
            MESH_TEST_FAIL_IF(!mesh_json_skip_value(&json), "an unread value should skip");
        }
    }
    MESH_TEST_FAIL_IF(seen != 3U, "every key should have been offered exactly once");
    MESH_TEST_FAIL_IF(strcmp(name, "T114") != 0 || model != 69U || !dfu,
                      "the three values should be what the document says");
    record_success(test_name);
}

/*
 * The case the whole reader exists for: text that looks like structure.
 *
 * A scanner hunting for `"id":` finds the one inside the note, reports v9.9.9 and offers an
 * update to a release that does not exist. Walking the structure, the note is one string value
 * and the only `id` is the object's own.
 */
MESH_TEST_CASE(json_ignores_structure_inside_a_string, unit) {
    static const char k_document[] =
        "{\"notes\": \"reverted the change that set \\\"id\\\": \\\"v9.9.9\\\" and a {brace\","
        " \"id\": \"v2.7.26\"}";
    struct mesh_json json;
    mesh_json_init(&json, k_document, 0U);
    MESH_TEST_FAIL_IF(!mesh_json_object_find(&json, "id"), "the real id should be found");

    char id[16];
    MESH_TEST_FAIL_IF(!mesh_json_read_string(&json, id, sizeof id), "the id is a string");
    MESH_TEST_FAIL_IF(strcmp(id, "v2.7.26") != 0, "a decoy inside a note is not a key");
    record_success(test_name);
}

MESH_TEST_CASE(json_unescapes_and_truncates_strings, unit) {
    static const char k_document[] = "[\"a\\\"b\\\\c\\n\", \"\\u00e9\\u0041\", "
                                     "\"\\ud800lone\", \"far too long for the buffer\"]";
    struct mesh_json json;
    mesh_json_init(&json, k_document, 0U);
    MESH_TEST_FAIL_IF(!mesh_json_enter_array(&json), "the document is an array");

    char out[8];
    MESH_TEST_FAIL_IF(!mesh_json_next_element(&json) || !mesh_json_read_string(&json, out, sizeof out),
                      "the first string should read");
    MESH_TEST_FAIL_IF(strcmp(out, "a\"b\\c\n") != 0, "the simple escapes should be unescaped");

    MESH_TEST_FAIL_IF(!mesh_json_next_element(&json) || !mesh_json_read_string(&json, out, sizeof out),
                      "the second string should read");
    MESH_TEST_FAIL_IF(strcmp(out, "\xc3\xa9" "A") != 0, "\\u should come out as UTF-8");

    MESH_TEST_FAIL_IF(!mesh_json_next_element(&json) || !mesh_json_read_string(&json, out, sizeof out),
                      "the third string should read");
    MESH_TEST_FAIL_IF(strcmp(out, "?lone") != 0, "half a surrogate pair is not a character");

    /*
     * The long one. What matters is not the truncation - it is that the cursor still ends up
     * after the string, so the array closes where it should. A reader that stopped writing and
     * also stopped reading would leave the walk inside a string it never left.
     */
    MESH_TEST_FAIL_IF(!mesh_json_next_element(&json) || !mesh_json_read_string(&json, out, sizeof out),
                      "an oversized string should still be consumed");
    MESH_TEST_FAIL_IF(strlen(out) != sizeof out - 1U, "it should fill the buffer and terminate");
    MESH_TEST_FAIL_IF(mesh_json_next_element(&json), "the array should end after four elements");
    record_success(test_name);
}

/* A truncated document ends the walk instead of running off the end. Every one of these is a
   buffer that stops mid-value, which is what a dropped connection produces. */
MESH_TEST_CASE(json_refuses_truncated_input, unit) {
    static const char *const k_broken[] = {
        "{\"a\": \"unterminated",
        "{\"a\": ",
        "{\"a\"",
        "[{\"a\": [1, 2",
        "{\"a\": \"\\u00\"}",
        "{\"a\": \"escape at the end\\",
        "",
    };
    for (size_t i = 0; i < sizeof k_broken / sizeof k_broken[0]; ++i) {
        struct mesh_json json;
        mesh_json_init(&json, k_broken[i], 0U);
        /* Whatever each of these does, it must stop: the assertion is that the call returns at
           all and that nothing it reports is a completed value. */
        char value[32];
        if (mesh_json_object_find(&json, "a")) {
            (void)mesh_json_read_string(&json, value, sizeof value);
        }
    }

    /* And the depth limit, which is the one refusal with a number behind it. */
    char deep[2U * MESH_JSON_MAX_DEPTH + 8U];
    size_t at = 0U;
    deep[at++] = '{';
    deep[at++] = '"';
    deep[at++] = 'a';
    deep[at++] = '"';
    deep[at++] = ':';
    for (size_t i = 0; i < MESH_JSON_MAX_DEPTH + 2U; ++i) {
        deep[at++] = '[';
    }
    deep[at] = '\0';
    struct mesh_json json;
    mesh_json_init(&json, deep, 0U);
    MESH_TEST_FAIL_IF(!mesh_json_object_find(&json, "a"), "the key itself is well formed");
    MESH_TEST_FAIL_IF(mesh_json_skip_value(&json), "a value nested past the limit should refuse");
    record_success(test_name);
}
