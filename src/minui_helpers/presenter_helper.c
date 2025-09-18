#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    size_t capacity = 0;
    size_t total = 0;
    char *buffer = NULL;

    while (1) {
        ssize_t len = getline(&buffer, &capacity, stdin);
        if (len < 0) {
            if (errno != 0 && errno != EINTR) {
                fprintf(stderr, "[minui-presenter] read error: %s\n", strerror(errno));
            }
            break;
        }
        total += (size_t)len;
        if (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            buffer[len - 1] = '\0';
            --len;
        }
        if (len > 0 && buffer[len - 1] == '\r') {
            buffer[len - 1] = '\0';
        }

        if (buffer[0] != '\0') {
            fprintf(stderr, "[minui-presenter] %s\n", buffer);
        }
    }

    if (total == 0) {
        fprintf(stderr, "[minui-presenter] (empty message)\n");
    }

    free(buffer);
    return 0;
}
