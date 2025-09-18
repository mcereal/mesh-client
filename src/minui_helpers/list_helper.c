#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char *line = NULL;
    size_t capacity = 0;
    size_t index = 0;

    while (1) {
        ssize_t len = getline(&line, &capacity, stdin);
        if (len < 0) {
            if (errno != 0 && errno != EINTR) {
                fprintf(stderr, "[minui-list] read error: %s\n", strerror(errno));
            }
            break;
        }
        if (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            --len;
        }
        if (len > 0 && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }

        fprintf(stderr, "[minui-list] %02zu: %s\n", index + 1, line);
        ++index;
    }

    free(line);
    return 0;
}
