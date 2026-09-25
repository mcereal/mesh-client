#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host file operations used by the map reader. Making a directory is inkwell's
   (inkwell_file_mkdir in inkwell/base/file.h). */
int mesh_file_open_readonly(const char *path);
bool mesh_file_read_at(int fd, uint8_t *buffer, size_t len, uint64_t offset);

#ifdef __cplusplus
}
#endif
