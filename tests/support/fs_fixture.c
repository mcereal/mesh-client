/* _XOPEN_SOURCE 700 rather than the suites' _POSIX_C_SOURCE: it is POSIX 2008 plus the XSI
   options, and nftw() is one of them. Both libcs this tree builds against have it. */
#define _XOPEN_SOURCE 700

#include "support/fs_fixture.h"

#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* How many descriptors nftw() may keep open while it descends. The fixture trees are a handful
   of levels deep; the walk still completes above this, it just reopens as it goes. */
#define MESH_TEST_FS_WALK_FDS 16

/*
 * nftw() has no user pointer, so what the callback could not remove is counted here. The suite
 * is single-threaded and one walk runs at a time - the same assumption every other fixture in
 * this directory makes.
 */
static int s_walk_failures;

/*
 * FTW_DEPTH visits a directory after its contents, so by the time this is called on one it is
 * already empty. FTW_PHYS keeps the walk off symlink targets - firmware_install.c's fixture
 * hangs the block device off a symlink into the USB tree, and following it would delete the
 * target through the link and leave the link itself behind.
 */
static int mesh_test_fs_remove_entry(const char *path, const struct stat *info, int type,
                                     struct FTW *level) {
    (void)info;
    (void)level;

    const int result = (type == FTW_DP) ? rmdir(path) : unlink(path);
    if (result != 0 && errno != ENOENT) {
        s_walk_failures++;
    }
    /* Keep going either way: a teardown that stops at the first stuck entry leaves more behind
       than one that clears what it can and reports at the end. */
    return 0;
}

bool mesh_test_remove_tree(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    s_walk_failures = 0;
    if (nftw(path, mesh_test_fs_remove_entry, MESH_TEST_FS_WALK_FDS, FTW_DEPTH | FTW_PHYS) != 0) {
        /* A path that is already gone is the answer a teardown running twice wants. */
        return errno == ENOENT;
    }
    return s_walk_failures == 0;
}

bool mesh_test_remove_matching(const char *dir, const char *prefix) {
    if (dir == NULL || prefix == NULL) {
        return false;
    }

    DIR *const handle = opendir(dir);
    if (handle == NULL) {
        return errno == ENOENT;
    }

    /*
     * The names are collected before anything is removed. Deleting entries out of a directory
     * that is being read is unspecified in POSIX, and a scan that quietly skipped a sibling
     * would leave a device half on the bus - which is the state these tests are asserting is
     * impossible.
     */
    const size_t prefix_len = strlen(prefix);
    char **matches = NULL;
    size_t count = 0;
    bool ok = true;

    const struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (strncmp(entry->d_name, prefix, prefix_len) != 0) {
            continue;
        }
        char **const grown = realloc(matches, (count + 1U) * sizeof *matches);
        if (grown == NULL) {
            ok = false;
            break;
        }
        matches = grown;
        matches[count] = strdup(entry->d_name);
        if (matches[count] == NULL) {
            ok = false;
            break;
        }
        count++;
    }
    (void)closedir(handle);

    for (size_t i = 0; i < count; ++i) {
        char child[PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", dir, matches[i]) >= (int)sizeof child) {
            ok = false;
        } else {
            ok = mesh_test_remove_tree(child) && ok;
        }
        free(matches[i]);
    }
    free(matches);

    return ok;
}
