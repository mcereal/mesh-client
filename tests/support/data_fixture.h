#ifndef MESH_TEST_DATA_FIXTURE_H
#define MESH_TEST_DATA_FIXTURE_H

#include <stddef.h>

/*
 * Reading the captured documents in tests/data/.
 *
 * A parser that reads bytes off the network is only tested by bytes that came off the network,
 * so the fixtures are the real documents rather than something written to suit the parser. They
 * are too big to be string literals and - once phase 2 arrives with a zip central directory -
 * not all of them are text, so they stay files and this opens them.
 *
 * The path is resolved against MESH_TEST_DATA_DIR, which tests/CMakeLists.txt defines as the
 * *source* directory. Nothing is copied at configure time, so a fixture refreshed on disk is
 * the one the next run reads.
 */

/* Reads tests/data/<name> whole. Returns a NUL-terminated buffer the caller frees, and sets
   `*len` (excluding the terminator) when it is not NULL. NULL when the file is unreadable. */
char *mesh_test_data_read(const char *name, size_t *len);

#endif /* MESH_TEST_DATA_FIXTURE_H */
