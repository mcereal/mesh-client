#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *consume_option(int *index, int argc, char **argv) {
    if (*index + 1 >= argc) {
        return NULL;
    }
    (*index)++;
    return argv[*index];
}

int main(int argc, char **argv) {
    const char *message = NULL;
    int timeout_ms = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--message") == 0) {
            message = consume_option(&i, argc, argv);
        } else if (strcmp(argv[i], "--timeout") == 0) {
            const char *value = consume_option(&i, argc, argv);
            if (value != NULL) {
                timeout_ms = atoi(value) * 1000;
            }
        }
    }

    if (message == NULL) {
        size_t capacity = 0;
        char *line = NULL;
        ssize_t len = getline(&line, &capacity, stdin);
        if (len > 0) {
            if (line[len - 1] == '\n') {
                line[len - 1] = '\0';
            }
            message = line;
        }
        if (message == NULL) {
            message = "";
        }
        fprintf(stderr, "[minui-presenter placeholder] %s\n",
                message[0] != '\0' ? message : "(empty message)");
        free(line);
    } else {
        fprintf(stderr, "[minui-presenter placeholder] %s\n",
                message[0] != '\0' ? message : "(empty message)");
    }

    if (timeout_ms > 0) {
        usleep((useconds_t)timeout_ms * 1000U);
    }

    return 0;
}
