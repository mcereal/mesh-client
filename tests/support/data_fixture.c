#include "support/data_fixture.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef MESH_TEST_DATA_DIR
#define MESH_TEST_DATA_DIR "tests/data"
#endif

char *mesh_test_data_read(const char *name, size_t *len) {
    if (len != NULL) {
        *len = 0U;
    }
    if (name == NULL) {
        return NULL;
    }
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", MESH_TEST_DATA_DIR, name) >= sizeof path) {
        return NULL;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *buffer = malloc((size_t)size + 1U);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    const size_t got = fread(buffer, 1U, (size_t)size, file);
    fclose(file);
    /* NUL-terminated whatever it is, so a caller that wants a C string has one and a caller
       that wants bytes has the length beside them. */
    buffer[got] = '\0';
    if (len != NULL) {
        *len = got;
    }
    return buffer;
}
