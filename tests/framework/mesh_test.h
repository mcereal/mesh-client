#ifndef MESH_TEST_H
#define MESH_TEST_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The suite is one binary assembled from many small translation units, one per subject area
 * under tests/suites/. A case registers itself from a constructor, so adding a test is a single
 * macro in a single file: there is no central table to edit, and no way to write a case that
 * silently never runs.
 *
 * This only works because the suite files are compiled straight into the executable. Rolling
 * them into a static library would let the linker drop the object files nothing references, and
 * every case inside them would disappear without a word. tests/CMakeLists.txt says the same.
 *
 * Constructors fire in unspecified order across translation units, so the runner sorts by
 * (file, line) before executing. Listing and run order are therefore source order within a
 * suite, and suites run alphabetically - stable regardless of how the linker felt that day.
 */

struct mesh_test_case {
    const char *name;
    const char *category;
    const char *file;
    int line;
    void (*fn)(void);
    struct mesh_test_case *next;
};

void mesh_test_register(struct mesh_test_case *node, const char *name, const char *category,
                        const char *file, int line, void (*fn)(void));

/* Outcome reporting: a case records exactly one of these and returns. */
void record_failure(const char *test_name, const char *message);
void record_success(const char *test_name);

/*
 * Defines a case and registers it. `case_name` is the bare name the runner filters on
 * (`--filter`, `--list`); `case_category` is the tag CTest labels select (`unit` today).
 *
 * The body is an ordinary function body with `test_name` already in scope, so the guard macros
 * below - and plain record_failure/record_success calls - work without repeating the name:
 *
 *     MESH_TEST_CASE(config_defaults, unit) {
 *         struct mesh_app_config config = mesh_app_config_default();
 *         MESH_TEST_FAIL_IF(config.idle_timeout_ms != 1000, "idle timeout should default to 1s");
 *         record_success(test_name);
 *     }
 */
#define MESH_TEST_CASE(case_name, case_category)                                                   \
    static void mesh_test_body_##case_name(const char *test_name);                                 \
    static void mesh_test_entry_##case_name(void) { mesh_test_body_##case_name(#case_name); }      \
    static void mesh_test_ctor_##case_name(void) __attribute__((constructor));                     \
    static void mesh_test_ctor_##case_name(void) {                                                 \
        static struct mesh_test_case node;                                                         \
        mesh_test_register(&node, #case_name, #case_category, __FILE__, __LINE__,                  \
                           mesh_test_entry_##case_name);                                           \
    }                                                                                              \
    static void mesh_test_body_##case_name(const char *test_name)

/* Records `message` against the running case and returns from it when `condition` holds. */
#define MESH_TEST_FAIL_IF(condition, message)                                                      \
    do {                                                                                           \
        if (condition) {                                                                           \
            record_failure(test_name, (message));                                                  \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/*
 * The same, for a case holding something that has to be released - an event loop, a mock, an
 * open fd. `cleanup` is a statement list run before the case gives up:
 *
 *     MESH_TEST_FAIL_IF_CLEANUP(result != 0, mesh_event_loop_shutdown(&loop), "start failed");
 */
#define MESH_TEST_FAIL_IF_CLEANUP(condition, cleanup, message)                                     \
    do {                                                                                           \
        if (condition) {                                                                           \
            cleanup;                                                                               \
            record_failure(test_name, (message));                                                  \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#endif /* MESH_TEST_H */
