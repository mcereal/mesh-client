#include "mesh/utils/file.h"

#include <stdio.h>
#include <stdlib.h>

uint8_t *mesh_file_read(const char *path, size_t max_len, size_t *out_len) {
    if (path == NULL || out_len == NULL) {
        return NULL;
    }
    *out_len = 0U;
    FILE *const file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long size = ftell(file);
    if (size <= 0 || (size_t)size > max_len || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *const bytes = malloc((size_t)size);
    if (bytes == NULL) {
        fclose(file);
        return NULL;
    }
    const size_t got = fread(bytes, 1U, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(bytes);
        return NULL;
    }
    *out_len = got;
    return bytes;
}
