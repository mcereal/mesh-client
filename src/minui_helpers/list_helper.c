#include <errno.h>
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

    FILE *input = stdin;
    if (file_path != NULL) {
        input = fopen(file_path, "r");
        if (input == NULL) {
            fprintf(stderr, "[minui-list placeholder] failed to open %s: %s\n", file_path, strerror(errno));
            return 1;
        }
    }

    fprintf(stderr, "[minui-list placeholder] rendering menu from %s\n",
            file_path != NULL ? file_path : "stdin");

    char buffer[512];
    while (fgets(buffer, sizeof buffer, input) != NULL) {
        fputs(buffer, stdout);
    }

    if (ferror(input) != 0) {
        fprintf(stderr, "[minui-list placeholder] read error: %s\n", strerror(errno));
    }

    if (input != stdin) {
        fclose(input);
    }

    fflush(stdout);
    return 0;
}
