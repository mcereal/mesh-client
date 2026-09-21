#define _POSIX_C_SOURCE 200809L

/*
 * Cutting the log on the card back.
 *
 * The property that matters most here is not the size - it is that the file keeps its inode.
 * `launch.sh` pipes the client through `tee -a`, so by the time anything in this module runs
 * there is already another process holding that file open and appending to it. A compaction that
 * renamed the file would leave `tee` writing into the rotated copy and the live path empty, which
 * is why `log_file_keeps_the_inode_so_an_open_writer_follows` and
 * `log_file_leaves_an_appending_writer_correct` are here: the first pins the mechanism, the
 * second pins the behaviour a reader of the log actually depends on.
 *
 * The path cases are the other half. The binary works the log's location out from `HOME` rather
 * than being told it, because `launch.sh` does not ship through self-update and the binary does -
 * so an install whose launcher has never heard of any of this still gets its log bounded.
 */

#include "framework/mesh_test.h"
#include "support/fs_fixture.h"

#include "mesh/utils/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A log of `lines` numbered lines, each padded to `width` so a case can predict the size it is
   building. Returns the file's size, or -1. */
static long log_file_write_lines(const char *path, unsigned lines, unsigned width) {
    FILE *file = fopen(path, "we");
    if (file == NULL) {
        return -1L;
    }
    for (unsigned i = 0; i < lines; ++i) {
        /* The number first, so a surviving line says which one it was. */
        int used = fprintf(file, "line-%08u", i);
        if (used < 0) {
            (void)fclose(file);
            return -1L;
        }
        for (unsigned pad = (unsigned)used; pad + 1U < width; ++pad) {
            (void)fputc('.', file);
        }
        (void)fputc('\n', file);
    }
    const bool failed = ferror(file) != 0;
    if (fclose(file) != 0 || failed) {
        return -1L;
    }
    struct stat info;
    return stat(path, &info) == 0 ? (long)info.st_size : -1L;
}

static long log_file_size(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 ? (long)info.st_size : -1L;
}

static bool log_file_first_line(const char *path, char *out, size_t out_len) {
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        return false;
    }
    const bool got = fgets(out, (int)out_len, file) != NULL;
    (void)fclose(file);
    return got;
}

/* Whether `needle` appears anywhere in the file. */
static bool log_file_contains(const char *path, const char *needle) {
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        return false;
    }
    char line[512];
    bool found = false;
    while (!found && fgets(line, (int)sizeof line, file) != NULL) {
        found = strstr(line, needle) != NULL;
    }
    (void)fclose(file);
    return found;
}

static bool log_file_tempdir(char *template_path) { return mkdtemp(template_path) != NULL; }

MESH_TEST_CASE(log_file_under_the_cap_is_left_alone, unit) {
    char dir[] = "/tmp/mesh_log_smallXXXXXX";
    MESH_TEST_FAIL_IF(!log_file_tempdir(dir), "mkdtemp failed");

    char path[256];
    snprintf(path, sizeof path, "%s/MeshClient.txt", dir);
    const long written = log_file_write_lines(path, 64U, 80U);
    MESH_TEST_FAIL_IF_CLEANUP(written <= 0, mesh_test_remove_tree(dir), "could not write a log");

    const long reclaimed = inkcell_log_file_compact(path);
    const long after = log_file_size(path);
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(reclaimed != 0L, "a log under the cap reported work it did not do");
    MESH_TEST_FAIL_IF(after != written, "a log under the cap was rewritten anyway");
    record_success(test_name);
}

MESH_TEST_CASE(log_file_over_the_cap_is_cut_to_its_newest_lines, unit) {
    char dir[] = "/tmp/mesh_log_bigXXXXXX";
    MESH_TEST_FAIL_IF(!log_file_tempdir(dir), "mkdtemp failed");

    char path[256];
    snprintf(path, sizeof path, "%s/MeshClient.txt", dir);
    /* Comfortably past the cap, so there is an oldest half with nothing to keep it. */
    const unsigned width = 80U;
    const unsigned lines = (unsigned)((INKCELL_LOG_FILE_MAX_BYTES / width) * 2U);
    const long written = log_file_write_lines(path, lines, width);
    MESH_TEST_FAIL_IF_CLEANUP(written <= (long)INKCELL_LOG_FILE_MAX_BYTES,
                              mesh_test_remove_tree(dir), "the fixture did not exceed the cap");

    /* The last line written, so "the newest survived" is asserted against a real line rather
       than against a number that drifts if the cap or the padding changes. */
    char newest[32];
    snprintf(newest, sizeof newest, "line-%08u", lines - 1U);

    const long reclaimed = inkcell_log_file_compact(path);
    const long after = log_file_size(path);
    const bool kept_newest = log_file_contains(path, newest);
    const bool dropped_oldest = !log_file_contains(path, "line-00000000");
    char first[512] = {0};
    const bool read_first = log_file_first_line(path, first, sizeof first);
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(reclaimed <= 0L, "an oversized log reported nothing reclaimed");
    MESH_TEST_FAIL_IF(after > (long)INKCELL_LOG_FILE_KEEP_BYTES,
                      "the log was not cut back to the retained window");
    MESH_TEST_FAIL_IF(after + reclaimed != written, "reclaimed does not account for the shrinkage");
    MESH_TEST_FAIL_IF(!kept_newest, "the newest line did not survive the compaction");
    MESH_TEST_FAIL_IF(!dropped_oldest, "the oldest line survived a compaction that should have "
                                       "cut it");
    MESH_TEST_FAIL_IF(!read_first, "the compacted log had no first line");
    /* The cut lands mid-line in the ordinary case and the remainder of that line is dropped, so
       the file never opens on half a sentence. */
    MESH_TEST_FAIL_IF(strncmp(first, "line-", 5U) != 0,
                      "the compacted log begins part-way through a line");
    record_success(test_name);
}

MESH_TEST_CASE(log_file_keeps_the_inode_so_an_open_writer_follows, unit) {
    char dir[] = "/tmp/mesh_log_inodeXXXXXX";
    MESH_TEST_FAIL_IF(!log_file_tempdir(dir), "mkdtemp failed");

    char path[256];
    snprintf(path, sizeof path, "%s/MeshClient.txt", dir);
    const unsigned width = 80U;
    const unsigned lines = (unsigned)((INKCELL_LOG_FILE_MAX_BYTES / width) * 2U);
    MESH_TEST_FAIL_IF_CLEANUP(log_file_write_lines(path, lines, width) <= 0,
                              mesh_test_remove_tree(dir), "could not write a log");

    struct stat before;
    const bool statted = stat(path, &before) == 0;
    const long reclaimed = inkcell_log_file_compact(path);
    struct stat after;
    const bool restatted = stat(path, &after) == 0;
    /* The rewrite works down one descriptor and writes nothing beside the log. This guards the
       earlier shape - a copy through `<path>.compact` - from coming back unnoticed. */
    char scratch[320];
    snprintf(scratch, sizeof scratch, "%s.compact", path);
    const bool scratch_gone = access(scratch, F_OK) != 0;
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(!statted || !restatted, "could not stat the log either side of a compaction");
    MESH_TEST_FAIL_IF(reclaimed <= 0L, "an oversized log reported nothing reclaimed");
    MESH_TEST_FAIL_IF(before.st_ino != after.st_ino,
                      "the compaction replaced the inode, so an open tee would follow the wrong "
                      "file");
    MESH_TEST_FAIL_IF(!scratch_gone, "the compaction left a stray file beside the log");
    record_success(test_name);
}

MESH_TEST_CASE(log_file_leaves_an_appending_writer_correct, unit) {
    char dir[] = "/tmp/mesh_log_appendXXXXXX";
    MESH_TEST_FAIL_IF(!log_file_tempdir(dir), "mkdtemp failed");

    char path[256];
    snprintf(path, sizeof path, "%s/MeshClient.txt", dir);
    const unsigned width = 80U;
    const unsigned lines = (unsigned)((INKCELL_LOG_FILE_MAX_BYTES / width) * 2U);
    MESH_TEST_FAIL_IF_CLEANUP(log_file_write_lines(path, lines, width) <= 0,
                              mesh_test_remove_tree(dir), "could not write a log");

    /* What `tee -a` is holding while the client starts: the same file, open for append. */
    const int writer = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
    MESH_TEST_FAIL_IF_CLEANUP(writer < 0, mesh_test_remove_tree(dir), "could not open an appender");

    const long reclaimed = inkcell_log_file_compact(path);

    static const char after_line[] = "after-the-compaction\n";
    const ssize_t put = write(writer, after_line, sizeof after_line - 1U);
    (void)close(writer);

    const long size = log_file_size(path);
    const bool landed = log_file_contains(path, "after-the-compaction");
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(reclaimed <= 0L, "an oversized log reported nothing reclaimed");
    MESH_TEST_FAIL_IF(put != (ssize_t)(sizeof after_line - 1U),
                      "the appender could not write after the compaction");
    MESH_TEST_FAIL_IF(!landed, "the appender's line did not reach the compacted log");
    /* O_APPEND positions each write at the current end, so the line lands after the retained
       tail rather than at the offset the writer held before - no hole, nothing lost. */
    MESH_TEST_FAIL_IF(size > (long)INKCELL_LOG_FILE_KEEP_BYTES + (long)sizeof after_line,
                      "the appender wrote past the end, which means it kept its old offset");
    record_success(test_name);
}

MESH_TEST_CASE(log_file_absent_is_not_an_error_and_is_not_created, unit) {
    char dir[] = "/tmp/mesh_log_absentXXXXXX";
    MESH_TEST_FAIL_IF(!log_file_tempdir(dir), "mkdtemp failed");

    char path[256];
    snprintf(path, sizeof path, "%s/never-was-a-log.txt", dir);
    const long reclaimed = inkcell_log_file_compact(path);
    const bool created = access(path, F_OK) == 0;
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(reclaimed != 0L, "a missing log was reported as a failure or as work done");
    MESH_TEST_FAIL_IF(created, "compacting a missing log created it");
    record_success(test_name);
}

MESH_TEST_CASE(log_file_one_enormous_line_is_left_alone, unit) {
    char dir[] = "/tmp/mesh_log_oneXXXXXX";
    MESH_TEST_FAIL_IF(!log_file_tempdir(dir), "mkdtemp failed");

    char path[256];
    snprintf(path, sizeof path, "%s/MeshClient.txt", dir);
    /* No newline anywhere in the retained window, so there is no line boundary to start from and
       nothing worth keeping. The file is left as it is rather than emptied. */
    const long written = log_file_write_lines(path, 1U, (unsigned)INKCELL_LOG_FILE_MAX_BYTES * 2U);
    MESH_TEST_FAIL_IF_CLEANUP(written <= (long)INKCELL_LOG_FILE_MAX_BYTES,
                              mesh_test_remove_tree(dir), "the fixture did not exceed the cap");

    const long reclaimed = inkcell_log_file_compact(path);
    const long after = log_file_size(path);
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(reclaimed != 0L, "a log with no line boundary reported work it cannot do");
    MESH_TEST_FAIL_IF(after != written, "a log with no line boundary was truncated anyway");
    record_success(test_name);
}

/*
 * A log path that is not an ordinary file is left exactly as it is.
 *
 * What this pins is the outcome, not which check produces it. A fifo reports `st_size` 0, so the
 * cap turns it away before the S_ISREG guard is reached, and a directory cannot be opened O_RDWR
 * at all - so S_ISREG is defence in depth here rather than the thing under test, and removing it
 * does not fail this case. The case is still worth its lines: the compaction truncates whatever
 * descriptor it opened, and "the log is a fifo" must not end in a replaced or emptied node.
 */
MESH_TEST_CASE(log_file_refuses_anything_but_a_regular_file, unit) {
    char dir[] = "/tmp/mesh_log_fifoXXXXXX";
    MESH_TEST_FAIL_IF(!log_file_tempdir(dir), "mkdtemp failed");

    char path[256];
    snprintf(path, sizeof path, "%s/MeshClient.txt", dir);
    const bool made = mkfifo(path, 0600) == 0;

    const long reclaimed = made ? inkcell_log_file_compact(path) : -1L;
    struct stat info;
    const bool still_a_fifo = stat(path, &info) == 0 && S_ISFIFO(info.st_mode);
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(!made, "could not create a fifo to stand in for a non-regular log");
    MESH_TEST_FAIL_IF(reclaimed != 0L, "a non-regular file was treated as a log to cut back");
    MESH_TEST_FAIL_IF(!still_a_fifo, "the compaction replaced a non-regular file");
    record_success(test_name);
}

MESH_TEST_CASE(log_file_path_is_derived_from_home, unit) {
    char *const saved_home = getenv("HOME");
    char home_copy[256] = {0};
    if (saved_home != NULL) {
        snprintf(home_copy, sizeof home_copy, "%s", saved_home);
    }
    (void)unsetenv("MESHCLIENT_LOG_FILE");

    /* What launch.sh exports: the pak's own userdata directory. The log is its sibling. */
    (void)setenv("HOME", "/mnt/SDCARD/.userdata/tg5040/MeshClient", 1);
    char path[INKCELL_LOG_FILE_PATH_MAX];
    const bool derived = inkcell_log_file_default_path(path, sizeof path);
    const bool matched =
        derived && strcmp(path, "/mnt/SDCARD/.userdata/tg5040/logs/MeshClient.txt") == 0;

    /* A trailing slash must not make the pak name empty. */
    (void)setenv("HOME", "/mnt/SDCARD/.userdata/tg5040/MeshClient/", 1);
    char slashed[INKCELL_LOG_FILE_PATH_MAX];
    const bool derived_slashed = inkcell_log_file_default_path(slashed, sizeof slashed);
    const bool matched_slashed =
        derived_slashed && strcmp(slashed, "/mnt/SDCARD/.userdata/tg5040/logs/MeshClient.txt") == 0;

    /* Nothing to derive from is an answer, not a guess. */
    (void)unsetenv("HOME");
    char unset[INKCELL_LOG_FILE_PATH_MAX];
    const bool derived_unset = inkcell_log_file_default_path(unset, sizeof unset);

    if (home_copy[0] != '\0') {
        (void)setenv("HOME", home_copy, 1);
    }

    MESH_TEST_FAIL_IF(!matched, "HOME did not derive the log beside the pak's userdata directory");
    MESH_TEST_FAIL_IF(!matched_slashed, "a trailing slash on HOME broke the derivation");
    MESH_TEST_FAIL_IF(derived_unset, "a path was claimed with no HOME to derive it from");
    MESH_TEST_FAIL_IF(unset[0] != '\0', "a failed derivation left something in the buffer");
    record_success(test_name);
}

/*
 * A HOME that is not the launcher's userdata directory derives nothing at all.
 *
 * The derivation is two components of guesswork - the log is HOME's parent plus `logs/` plus
 * HOME's own name - and on an ordinary host that names a real place: `HOME=/srv/users/alice`
 * gives `/srv/users/logs/alice.txt`. Deriving it there and then compacting it would cut back a
 * file the client has nothing to do with, so the shape `launch.sh` builds is required before the
 * path is offered at all.
 */
MESH_TEST_CASE(log_file_path_refuses_a_home_that_is_not_the_pak_userdata_dir, unit) {
    char *const saved_home = getenv("HOME");
    char home_copy[256] = {0};
    if (saved_home != NULL) {
        snprintf(home_copy, sizeof home_copy, "%s", saved_home);
    }
    (void)unsetenv("MESHCLIENT_LOG_FILE");

    /* Each of these has the two components the derivation needs and is still not a pak's
       userdata directory. */
    static const char *const strangers[] = {
        "/srv/users/alice",                       /* the reviewer's case: a real host layout */
        "/home/user",                             /* a developer box */
        "/mnt/SDCARD/userdata/tg5040/MeshClient", /* userdata, but not the dot-directory */
        "/mnt/SDCARD/.userdata/MeshClient",       /* no platform component between them */
        "/tmp",                                   /* one component */
    };

    bool derived_any = false;
    const char *offender = NULL;
    for (size_t i = 0; i < sizeof strangers / sizeof strangers[0]; ++i) {
        (void)setenv("HOME", strangers[i], 1);
        char path[INKCELL_LOG_FILE_PATH_MAX];
        if (inkcell_log_file_default_path(path, sizeof path)) {
            derived_any = true;
            offender = strangers[i];
            break;
        }
    }

    /* The device layout still works, so the guard is a shape check and not a refusal to derive. */
    (void)setenv("HOME", "/mnt/SDCARD/.userdata/tg5040/MeshClient", 1);
    char device[INKCELL_LOG_FILE_PATH_MAX];
    const bool device_ok = inkcell_log_file_default_path(device, sizeof device) &&
                           strcmp(device, "/mnt/SDCARD/.userdata/tg5040/logs/MeshClient.txt") == 0;

    /* And an explicit override is not subject to the shape at all. */
    (void)setenv("HOME", "/home/user", 1);
    (void)setenv("MESHCLIENT_LOG_FILE", "/tmp/anywhere.txt", 1);
    char override[INKCELL_LOG_FILE_PATH_MAX];
    const bool override_ok = inkcell_log_file_default_path(override, sizeof override) &&
                             strcmp(override, "/tmp/anywhere.txt") == 0;

    (void)unsetenv("MESHCLIENT_LOG_FILE");
    if (home_copy[0] != '\0') {
        (void)setenv("HOME", home_copy, 1);
    }

    MESH_TEST_FAIL_IF(derived_any && offender != NULL,
                      "a HOME outside the pak's userdata directory still derived a log path");
    MESH_TEST_FAIL_IF(!device_ok, "the real device layout stopped deriving its log");
    MESH_TEST_FAIL_IF(!override_ok, "MESHCLIENT_LOG_FILE was made subject to the HOME shape");
    record_success(test_name);
}

MESH_TEST_CASE(log_file_path_override_wins, unit) {
    char *const saved_home = getenv("HOME");
    char home_copy[256] = {0};
    if (saved_home != NULL) {
        snprintf(home_copy, sizeof home_copy, "%s", saved_home);
    }

    (void)setenv("HOME", "/mnt/SDCARD/.userdata/tg5040/MeshClient", 1);
    (void)setenv("MESHCLIENT_LOG_FILE", "/tmp/somewhere-else.txt", 1);
    char path[INKCELL_LOG_FILE_PATH_MAX];
    const bool derived = inkcell_log_file_default_path(path, sizeof path);
    const bool matched = derived && strcmp(path, "/tmp/somewhere-else.txt") == 0;

    (void)unsetenv("MESHCLIENT_LOG_FILE");
    if (home_copy[0] != '\0') {
        (void)setenv("HOME", home_copy, 1);
    }

    MESH_TEST_FAIL_IF(!matched, "MESHCLIENT_LOG_FILE did not override the derived path");
    record_success(test_name);
}
