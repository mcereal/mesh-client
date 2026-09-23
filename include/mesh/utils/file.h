#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host file operations used by the map reader and per-user stores. mkdir uses mode 0700 on
   POSIX and inherits the parent directory's ACL on Windows. */
int mesh_file_open_readonly(const char *path);
bool mesh_file_read_at(int fd, uint8_t *buffer, size_t len, uint64_t offset);
int mesh_file_mkdir(const char *path);

#ifdef __cplusplus
}
#endif
