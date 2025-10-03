#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *consume_option(int *index, int argc, char **argv) {
    if (*index + 1 >= argc) {
        return NULL;
    }
    (*index)++;
    return argv[*index];
}

static char *slurp(FILE *input, size_t *length_out) {
    size_t capacity = 4096U;
    char *buffer = (char *)malloc(capacity + 1U);
    if (buffer == NULL) {
        return NULL;
    }

    size_t length = 0U;
    while (true) {
        size_t remaining = capacity - length;
        size_t read_len = fread(buffer + length, 1U, remaining, input);
        length += read_len;
        if (read_len < remaining) {
            if (ferror(input) != 0) {
                free(buffer);
                return NULL;
            }
            break;
        }
        capacity *= 2U;
        char *next = (char *)realloc(buffer, capacity + 1U);
        if (next == NULL) {
            free(buffer);
            return NULL;
        }
        buffer = next;
    }

    buffer[length] = '\0';
    if (length_out != NULL) {
        *length_out = length;
    }
    return buffer;
}

static char *read_json_payload(const char *file_path, size_t *length_out) {
    if (file_path == NULL) {
        return slurp(stdin, length_out);
    }

    FILE *file = fopen(file_path, "r");
    if (file == NULL) {
        return NULL;
    }

    char *payload = slurp(file, length_out);
    fclose(file);
    return payload;
}

static size_t count_device_options(const char *json) {
    if (json == NULL) {
        return 0U;
    }

    const char *options = strstr(json, "\"options\"");
    if (options == NULL) {
        return 0U;
    }

    options = strchr(options, '[');
    if (options == NULL) {
        return 0U;
    }
    ++options;

    size_t count = 0U;
    bool in_string = false;
    bool escape = false;

    for (const char *cursor = options; *cursor != '\0'; ++cursor) {
        const char ch = *cursor;
        if (escape) {
            escape = false;
            continue;
        }
        if (ch == '\\') {
            if (in_string) {
                escape = true;
            }
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            if (in_string) {
                ++count;
            }
            continue;
        }
        if (!in_string && ch == ']') {
            break;
        }
    }

    return count;
}

static int parse_default_selection(const char *json) {
    if (json == NULL) {
        return 0;
    }

    const char *selected = strstr(json, "\"selected\"");
    if (selected == NULL) {
        return 0;
    }

    selected = strchr(selected, ':');
    if (selected == NULL) {
        return 0;
    }
    ++selected;

    while (*selected != '\0' && isspace((unsigned char)*selected)) {
        ++selected;
    }

    errno = 0;
    char *endptr = NULL;
    long parsed = strtol(selected, &endptr, 10);
    if (errno != 0 || endptr == selected) {
        return 0;
    }
    if (parsed < 0) {
        parsed = 0;
    }
    if (parsed > INT_MAX) {
        parsed = INT_MAX;
    }
    return (int)parsed;
}

static bool env_override(int *selection) {
    const char *override_value = getenv("MESHCLIENT_MINUI_SELECTION");
    if (override_value == NULL || override_value[0] == '\0') {
        return false;
    }

    errno = 0;
    char *endptr = NULL;
    long parsed = strtol(override_value, &endptr, 10);
    if (errno != 0 || endptr == override_value || *endptr != '\0') {
        return false;
    }
    if (parsed < 0) {
        parsed = 0;
    }
    if (parsed > INT_MAX) {
        parsed = INT_MAX;
    }
    *selection = (int)parsed;
    return true;
}

static int clamp_selection(int selection, size_t option_count) {
    if (option_count == 0U) {
        return 0;
    }
    if (selection < 0) {
        return 0;
    }
    if ((size_t)selection >= option_count) {
        return (int)(option_count - 1U);
    }
    return selection;
}

int main(int argc, char **argv) {
    const char *file_path = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--file") == 0) {
            file_path = consume_option(&i, argc, argv);
        } else if (strcmp(argv[i], "--format") == 0 || strcmp(argv[i], "--title") == 0 ||
                   strcmp(argv[i], "--confirm-text") == 0 || strcmp(argv[i], "--item-key") == 0 ||
                   strcmp(argv[i], "--write-value") == 0) {
            (void)consume_option(&i, argc, argv);
        } else if (strcmp(argv[i], "--disable-auto-sleep") == 0) {
            continue;
        }
    }

    size_t json_length = 0U;
    char *json = read_json_payload(file_path, &json_length);
    if (json == NULL) {
        fprintf(stderr, "[minui-list placeholder] failed to read menu payload%s%s\n",
                file_path != NULL ? " from " : "",
                file_path != NULL ? file_path : "");
        return 1;
    }

    fprintf(stderr, "[minui-list placeholder] menu payload (%zu bytes)\n", json_length);

    size_t option_count = count_device_options(json);
    int selection = parse_default_selection(json);
    int original_selection = selection;
    if (env_override(&selection)) {
        fprintf(stderr, "[minui-list placeholder] override selection via MESHCLIENT_MINUI_SELECTION=%d\n",
                selection);
    }

    selection = clamp_selection(selection, option_count);
    fprintf(stderr, "[minui-list placeholder] selecting index %d (default %d, options=%zu)\n", selection,
            original_selection, option_count);

    printf("{\"settings\":{\"selected\":%d}}\n", selection);
    fflush(stdout);

    free(json);
    return 0;
}
