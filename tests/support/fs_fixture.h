#ifndef MESH_TEST_SUPPORT_FS_FIXTURE_H
#define MESH_TEST_SUPPORT_FS_FIXTURE_H

/*
 * Taking down a tree a suite laid out on disk.
 *
 * Two suites build a fake /sys out of real directories - transport_serial.c to enumerate USB
 * nodes, firmware_install.c to walk a radio into its bootloader and back - and both had the
 * same `system("rm -rf '...'")` to clear it again. That is what these replace, and the reason
 * is not only the duplication: system() is declared warn_unused_result, so the release build
 * (where _FORTIFY_SOURCE turns the attribute on) warned on every ignored call, and checking the
 * result would not have said much anyway - a shell that could not be started and a shell that
 * ran and failed come back the same way.
 *
 * Walking the tree ourselves gives an answer worth reading, needs no /bin/sh under the test
 * binary, and takes the quoting question away with it.
 */

#include <stdbool.h>

/*
 * Remove `path` and everything under it. True when nothing is left there afterwards, which
 * includes a path that was already gone - a teardown that runs twice is not a failure.
 */
bool mesh_test_remove_tree(const char *path);

/*
 * Remove the entries of `dir` whose names begin with `prefix`, each as a tree of its own. This
 * is the glob the suites used to hand the shell ("2-1*"): one USB device leaving the bus, which
 * is several sysfs directories that share a name. True when every match is gone, and when there
 * was no match to begin with.
 */
bool mesh_test_remove_matching(const char *dir, const char *prefix);

#endif /* MESH_TEST_SUPPORT_FS_FIXTURE_H */
