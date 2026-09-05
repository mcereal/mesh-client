#define _POSIX_C_SOURCE 200809L

#include "framework/mesh_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESH_TEST_SUITE_NAME_MAX 128

static struct mesh_test_case *g_cases = NULL;
static size_t g_case_count = 0U;
static int g_failures = 0;
static size_t g_successes = 0U;

void mesh_test_register(struct mesh_test_case *node, const char *name, const char *category,
                        const char *file, int line, void (*fn)(void)) {
    node->name = name;
    node->category = category;
    node->file = file;
    node->line = line;
    node->fn = fn;
    node->next = g_cases;
    g_cases = node;
    ++g_case_count;
}

void record_failure(const char *test_name, const char *message) {
    fprintf(stderr, "[FAIL] %s: %s\n", test_name, message);
    ++g_failures;
}

void record_success(const char *test_name) {
    (void)test_name;
    ++g_successes;
}

/*
 * The suite a case lives in: its file name with the directory and the .c stripped. Written into
 * a caller-supplied buffer so two suite names can be compared without one clobbering the other.
 */
static const char *suite_of(const struct mesh_test_case *test, char *out, size_t cap) {
    const char *slash = strrchr(test->file, '/');
    const char *base = (slash != NULL) ? slash + 1 : test->file;
    size_t len = strlen(base);
    if (len >= 2U && strcmp(base + len - 2, ".c") == 0) {
        len -= 2U;
    }
    if (len >= cap) {
        len = cap - 1U;
    }
    memcpy(out, base, len);
    out[len] = '\0';
    return out;
}

/* Constructor order is unspecified, so impose source order: suite name, then line. */
static int compare_cases(const void *lhs, const void *rhs) {
    const struct mesh_test_case *const *left = lhs;
    const struct mesh_test_case *const *right = rhs;

    char left_suite[MESH_TEST_SUITE_NAME_MAX];
    char right_suite[MESH_TEST_SUITE_NAME_MAX];
    const int by_suite = strcmp(suite_of(*left, left_suite, sizeof(left_suite)),
                                suite_of(*right, right_suite, sizeof(right_suite)));
    if (by_suite != 0) {
        return by_suite;
    }
    if ((*left)->line != (*right)->line) {
        return ((*left)->line < (*right)->line) ? -1 : 1;
    }
    return 0;
}

/* Flattens the registration list into source order. Caller frees. */
static struct mesh_test_case **ordered_cases(size_t *count) {
    *count = g_case_count;
    if (g_case_count == 0U) {
        return NULL;
    }

    struct mesh_test_case **ordered = calloc(g_case_count, sizeof(*ordered));
    if (ordered == NULL) {
        fprintf(stderr, "out of memory ordering %zu test cases\n", g_case_count);
        exit(1);
    }

    size_t index = 0U;
    for (struct mesh_test_case *node = g_cases; node != NULL; node = node->next) {
        ordered[index++] = node;
    }
    qsort(ordered, g_case_count, sizeof(*ordered), compare_cases);
    return ordered;
}

struct test_options {
    const char *category;
    const char *name_filter;
    const char *suite_filter;
    bool list_only;
};

static bool string_matches_filter(const char *value, const char *filter) {
    if (filter == NULL || filter[0] == '\0') {
        return true;
    }
    if (value == NULL) {
        return false;
    }
    return strstr(value, filter) != NULL;
}

static bool test_selected(const struct mesh_test_case *test, const struct test_options *options) {
    if (options->category != NULL && options->category[0] != '\0' &&
        strcmp(options->category, test->category) != 0) {
        return false;
    }
    if (!string_matches_filter(test->name, options->name_filter)) {
        return false;
    }
    char suite[MESH_TEST_SUITE_NAME_MAX];
    return string_matches_filter(suite_of(test, suite, sizeof(suite)), options->suite_filter);
}

static void print_available_tests(void) {
    size_t count = 0U;
    struct mesh_test_case **ordered = ordered_cases(&count);
    printf("Available tests (%zu):\n", count);
    for (size_t i = 0; i < count; ++i) {
        char suite[MESH_TEST_SUITE_NAME_MAX];
        printf("  - %-40s [%s] %s\n", ordered[i]->name, ordered[i]->category,
               suite_of(ordered[i], suite, sizeof(suite)));
    }
    free(ordered);
}

static void select_tests(const struct test_options *options, size_t *selected, bool *ran_any) {
    size_t count = 0U;
    struct mesh_test_case **ordered = ordered_cases(&count);
    size_t executed = 0U;
    size_t registered = 0U;

    for (size_t i = 0; i < count; ++i) {
        const struct mesh_test_case *test = ordered[i];
        if (!test_selected(test, options)) {
            continue;
        }

        ++registered;
        if (options->list_only) {
            char suite[MESH_TEST_SUITE_NAME_MAX];
            printf("%-40s [%s] %s\n", test->name, test->category,
                   suite_of(test, suite, sizeof(suite)));
            continue;
        }

        fprintf(stderr, "[RUN] %s (%s)\n", test->name, test->category);
        test->fn();
        ++executed;
    }

    free(ordered);

    if (selected != NULL) {
        *selected = registered;
    }
    if (ran_any != NULL) {
        *ran_any = (executed > 0U);
    }
}

static void print_summary(size_t selected, bool ran_any) {
    if (selected == 0U) {
        printf("No tests matched the provided filters.\n");
        return;
    }

    if (!ran_any) {
        return;
    }

    const size_t passed = g_successes;
    const size_t failed = (size_t)g_failures;
    const size_t total = passed + failed;

    printf("Summary: %zu executed (%zu passed, %zu failed)\n", total, passed, failed);
}

static void print_usage(void) {
    printf("Usage: meshclient_core_tests [options]\n");
    printf("Options:\n");
    printf("  --category NAME   Run only tests in category NAME (e.g. unit, integration).\n");
    printf("  --filter SUBSTR   Run tests whose name contains SUBSTR.\n");
    printf("  --suite SUBSTR    Run tests from suite files whose name contains SUBSTR.\n");
    printf("  --list            List matching tests without executing them.\n");
    printf("  --help            Show this help message.\n");
}

int main(int argc, char **argv) {
    struct test_options options = {
        .category = NULL,
        .name_filter = NULL,
        .suite_filter = NULL,
        .list_only = false,
    };

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--category") == 0) {
            if (i + 1 < argc) {
                options.category = argv[++i];
            } else {
                fprintf(stderr, "--category requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--filter") == 0) {
            if (i + 1 < argc) {
                options.name_filter = argv[++i];
            } else {
                fprintf(stderr, "--filter requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--suite") == 0) {
            if (i + 1 < argc) {
                options.suite_filter = argv[++i];
            } else {
                fprintf(stderr, "--suite requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--list") == 0) {
            options.list_only = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage();
            print_available_tests();
            return 0;
        } else if (strcmp(argv[i], "--all") == 0) {
            options.category = NULL;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage();
            return 1;
        }
    }

    if (options.list_only) {
        select_tests(&options, NULL, NULL);
        return 0;
    }

    size_t selected = 0U;
    bool ran_any = false;
    select_tests(&options, &selected, &ran_any);
    print_summary(selected, ran_any);

    if (g_failures > 0) {
        fprintf(stderr, "Tests failed: %d\n", g_failures);
        return 1;
    }

    if (!ran_any) {
        printf("No tests were executed.\n");
    }

    return 0;
}
