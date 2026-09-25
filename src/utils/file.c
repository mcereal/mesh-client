#define _POSIX_C_SOURCE 200809L

#include "mesh/utils/file.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(_WIN32)
#include <io.h>
#endif

int mesh_file_open_readonly(const char *path) {
    if (path == NULL) {
        return -EINVAL;
    }
#if defined(_WIN32)
    const int fd = _open(path, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
#else
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
    return fd < 0 ? -errno : fd;
}

bool mesh_file_read_at(int fd, uint8_t *buffer, size_t len, uint64_t offset) {
    if (fd < 0 || (buffer == NULL && len != 0U)) {
        return false;
    }
    size_t got = 0U;
    while (got < len) {
#if defined(_WIN32)
        /* The map reader uses this descriptor only on the loop thread. Windows CRT descriptors
           have a cursor, unlike pread, so seek before each chunk and keep the file binary. */
        if (offset > (uint64_t)INT64_MAX || got > (uint64_t)INT64_MAX - offset ||
            _lseeki64(fd, (__int64)(offset + got), SEEK_SET) < 0) {
            return false;
        }
        const size_t chunk = len - got < (size_t)INT_MAX ? len - got : (size_t)INT_MAX;
        const int n = _read(fd, buffer + got, (unsigned)chunk);
#else
        const ssize_t n = pread(fd, buffer + got, len - got, (off_t)(offset + got));
#endif
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}
