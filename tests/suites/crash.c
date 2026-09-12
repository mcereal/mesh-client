#define _POSIX_C_SOURCE 200809L

/*
 * The crash reporter.
 *
 * Split in two on purpose, because half of this module cannot be tested the way the rest of the
 * suite tests anything.
 *
 * The **writer** is ordinary code and is checked in-process through mesh_crash_write_report():
 * the notes come out under their headings, the log ring keeps the newest lines in the order they
 * were logged, and the file says the things a reader has to be able to find in it.
 *
 * The **handler** cannot be. It is installed process-wide, it is reached only by a real fault,
 * and the fault it is reached by kills whatever raised it - so a case that raised SIGSEGV in the
 * test binary would take the runner down with it, and one that called the handler directly would
 * be checking a function rather than the thing that matters, which is whether a *crashing
 * process* leaves a file behind. So those cases fork: the child installs, faults for real, and
 * dies, and the parent reads the file the child left and the status the kernel reported.
 *
 * The forking is not only about surviving the fault. A signal disposition is per process and so
 * is everything else in that module, and this binary runs the whole suite in one process - the
 * app cases install a handler of their own long before these run, since mesh_app_init() does.
 * A child gets its own copy of those statics, so each case here starts from a state it controls
 * rather than from whatever ran first alphabetically. That ordering is exactly what caught the
 * install bug `crash_install_re_aims_rather_than_ignoring_a_second_call` now pins.
 */

#include "framework/mesh_test.h"
#include "support/fs_fixture.h"

#include "mesh/utils/crash.h"
#include "mesh/utils/log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Read a whole file into `out`. False when it is missing or bigger than the buffer, which for a
   report means something is wrong with it rather than with the reading. */
static bool crash_test_slurp(const char *path, char *out, size_t out_len) {
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        return false;
    }
    const size_t read = fread(out, 1U, out_len - 1U, file);
    out[read] = '\0';
    const bool complete = feof(file) != 0;
    (void)fclose(file);
    return complete;
}

/* A directory of this case's own under /tmp, in the shape the rest of the suite uses. */
static bool crash_test_tempdir(char *template_path) { return mkdtemp(template_path) != NULL; }

/* ---- the writer ------------------------------------------------------------------------------ */

MESH_TEST_CASE(crash_report_carries_its_notes, unit) {
    mesh_crash_note(MESH_CRASH_NOTE_VERSION, "9.9.9-test");
    mesh_crash_note(MESH_CRASH_NOTE_ROUTE, "nodes/map");
    mesh_crash_note(MESH_CRASH_NOTE_TRANSPORT, "connected: Test Radio");

    char dir[] = "/tmp/mesh_crash_notesXXXXXX";
    MESH_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    char path[256];
    snprintf(path, sizeof path, "%s/report.txt", dir);
    FILE *file = fopen(path, "we");
    MESH_TEST_FAIL_IF_CLEANUP(file == NULL, mesh_test_remove_tree(dir), "could not open a report");
    mesh_crash_write_report(fileno(file), 11);
    (void)fclose(file);

    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(!read, "report was unreadable or longer than the buffer");

    MESH_TEST_FAIL_IF(strstr(body, "9.9.9-test") == NULL, "the version note is missing");
    MESH_TEST_FAIL_IF(strstr(body, "nodes/map") == NULL, "the route note is missing");
    MESH_TEST_FAIL_IF(strstr(body, "connected: Test Radio") == NULL,
                      "the transport note is missing");
    /* The signal is named as well as numbered: a bug report quoting "11" and one quoting
       "SIGSEGV" should not be two different conversations. */
    MESH_TEST_FAIL_IF(strstr(body, "SIGSEGV") == NULL, "the signal was not named");

    /*
     * The promise the file opens with is load-bearing rather than decorative. It is what a user
     * reads before deciding whether to attach the thing to a public issue, so a change that
     * started putting message text or coordinates in here has to fail something.
     */
    MESH_TEST_FAIL_IF(strstr(body, "no message") == NULL,
                      "the report no longer says what it leaves out");
    record_success(test_name);
}

MESH_TEST_CASE(crash_report_keeps_the_newest_log_lines, unit) {
    /*
     * Two full turns of the ring plus a bit, so nothing any other case logged can still be in
     * it - which is what makes the assertions below about *absence* mean anything in a binary
     * where every other suite is logging as it runs.
     */
    const unsigned pushed = MESH_CRASH_LOG_LINES * 2U + 3U;
    for (unsigned i = 0U; i < pushed; ++i) {
        char line[64];
        snprintf(line, sizeof line, "ring line %u", i);
        mesh_crash_log_line(line);
    }

    char dir[] = "/tmp/mesh_crash_ringXXXXXX";
    MESH_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");
    char path[256];
    snprintf(path, sizeof path, "%s/report.txt", dir);
    FILE *file = fopen(path, "we");
    MESH_TEST_FAIL_IF_CLEANUP(file == NULL, mesh_test_remove_tree(dir), "could not open a report");
    mesh_crash_write_report(fileno(file), 6);
    (void)fclose(file);

    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(!read, "report was unreadable or longer than the buffer");

    /* The last line pushed is the last line in the file: what was happening immediately before
       the fault is the whole reason the ring exists. */
    char newest[64];
    snprintf(newest, sizeof newest, "ring line %u", pushed - 1U);
    MESH_TEST_FAIL_IF(strstr(body, newest) == NULL, "the newest line is not in the report");

    /* And the line that fell off the far end is gone rather than lingering. */
    char evicted[64];
    snprintf(evicted, sizeof evicted, "ring line %u\n", pushed - MESH_CRASH_LOG_LINES - 1U);
    MESH_TEST_FAIL_IF(strstr(body, evicted) != NULL, "an evicted line is still in the report");

    /*
     * Oldest first. A ring written newest-first reads as a stack trace run backwards, and the
     * one thing somebody does with this section is start at the bottom - so the order is part of
     * the format rather than an implementation detail.
     */
    char oldest[64];
    snprintf(oldest, sizeof oldest, "ring line %u\n", pushed - MESH_CRASH_LOG_LINES);
    const char *first = strstr(body, oldest);
    const char *last = strstr(body, newest);
    MESH_TEST_FAIL_IF(first == NULL, "the oldest surviving line is missing");
    MESH_TEST_FAIL_IF(last == NULL || first > last, "the log is not in the order it was written");
    record_success(test_name);
}

MESH_TEST_CASE(crash_log_ring_takes_what_the_logger_formats, unit) {
    /*
     * The tap is in mesh_log_message_v(), so a line reaches the ring already carrying its level
     * and its component. Checked through the logger rather than through mesh_crash_log_line()
     * directly, because what would break here is the wiring rather than the ring.
     */
    const enum mesh_log_level restore = mesh_log_get_level();
    mesh_log_set_level(MESH_LOG_LEVEL_INFO);
    mesh_log_info("crashtest", "a marker worth %d cents", 42);

    char dir[] = "/tmp/mesh_crash_tapXXXXXX";
    MESH_TEST_FAIL_IF_CLEANUP(!crash_test_tempdir(dir), mesh_log_set_level(restore),
                              "mkdtemp failed");
    char path[256];
    snprintf(path, sizeof path, "%s/report.txt", dir);
    FILE *file = fopen(path, "we");
    MESH_TEST_FAIL_IF_CLEANUP(file == NULL, mesh_log_set_level(restore);
                              mesh_test_remove_tree(dir), "could not open a report");
    mesh_crash_write_report(fileno(file), 6);
    (void)fclose(file);

    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    mesh_test_remove_tree(dir);
    mesh_log_set_level(restore);
    MESH_TEST_FAIL_IF(!read, "report was unreadable or longer than the buffer");
    MESH_TEST_FAIL_IF(strstr(body, "a marker worth 42 cents") == NULL,
                      "the logger's line never reached the ring");
    MESH_TEST_FAIL_IF(strstr(body, "(crashtest)") == NULL, "the component was dropped on the way");

    /*
     * Below the level in force, so the log file never showed it and the ring must not either.
     * The two have to agree: a report holding lines the log does not is a report that disagrees
     * with the file it is meant to be pasted beside.
     */
    mesh_log_set_level(MESH_LOG_LEVEL_ERROR);
    mesh_log_debug("crashtest", "this line is below the level");
    mesh_log_set_level(restore);

    char second[256];
    snprintf(second, sizeof second, "%s", path);
    char dir2[] = "/tmp/mesh_crash_tap2XXXXXX";
    MESH_TEST_FAIL_IF(!crash_test_tempdir(dir2), "mkdtemp failed");
    snprintf(second, sizeof second, "%s/report.txt", dir2);
    FILE *again = fopen(second, "we");
    MESH_TEST_FAIL_IF_CLEANUP(again == NULL, mesh_test_remove_tree(dir2), "could not open");
    mesh_crash_write_report(fileno(again), 6);
    (void)fclose(again);
    const bool read2 = crash_test_slurp(second, body, sizeof body);
    mesh_test_remove_tree(dir2);
    MESH_TEST_FAIL_IF(!read2, "second report was unreadable");
    MESH_TEST_FAIL_IF(strstr(body, "below the level") != NULL,
                      "a line the log filtered out reached the ring");
    record_success(test_name);
}

/* ---- the handler, from a process that really crashes ------------------------------------------
 */

/*
 * Fault in a child and report how it went.
 *
 * `mode` picks what the child does once it has installed. The child never returns: it either
 * dies of the signal - which is the case under test - or _exit()s with a code the parent can
 * tell apart from a signal death.
 */
enum crash_child_mode {
    CRASH_CHILD_SEGV = 0,
    CRASH_CHILD_ABORT,
    CRASH_CHILD_CLEAN,
};

/*
 * Four calls deep, so a walk that stops early is visibly wrong rather than plausibly short. The
 * `volatile` argument is what stops the compiler folding the three together and handing the walk
 * one frame to find.
 *
 * The fault is `raise()` rather than a dereference of NULL, and that is not squeamishness. A
 * null dereference is undefined behaviour, which means a sanitizer is entitled to do something
 * other than let it fault - and UBSan does exactly that: by default it *reports* the load and
 * lets the program carry on, so under the sanitizer build these children never died at all and
 * every case here failed on a report that was never written. Asking for the signal directly is
 * what the handler's contract is actually about - a fatal signal arrives, a report is written,
 * and the process still dies of it - and it behaves the same under every build this repo makes.
 *
 * What the raise does not exercise is a genuinely bad `si_addr` on a genuinely damaged stack;
 * nothing here asserts on either, and a real dereference was used by hand to confirm the report
 * resolves through addr2line to the faulting line.
 */
static int crash_child_three(volatile int *p) {
    (void)p;
    raise(SIGSEGV);
    return 0;
}
static int crash_child_two(volatile int *p) { return crash_child_three(p) + 1; }
static int crash_child_one(volatile int *p) { return crash_child_two(p) + 1; }

static pid_t crash_test_fork_child(const char *dir, enum crash_child_mode mode) {
    const pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }

    if (mesh_crash_install(dir) != 0) {
        _exit(40);
    }
    mesh_crash_note(MESH_CRASH_NOTE_VERSION, "child-build");
    mesh_crash_note(MESH_CRASH_NOTE_ROUTE, "status/trend");
    mesh_log_set_level(MESH_LOG_LEVEL_INFO);
    mesh_log_info("child", "the last thing the child did");

    switch (mode) {
    case CRASH_CHILD_SEGV:
        /* Reached only if the signal did not kill us, which is itself a failure worth a code of
           its own rather than a silent pass. */
        _exit(crash_child_one((volatile int *)0) == 0 ? 41 : 42);
    case CRASH_CHILD_ABORT:
        abort();
    case CRASH_CHILD_CLEAN:
    default:
        _exit(mesh_crash_report_waiting() ? 1 : 0);
    }
}

MESH_TEST_CASE(crash_handler_writes_a_report_from_a_real_fault, unit) {
    char dir[] = "/tmp/mesh_crash_faultXXXXXX";
    MESH_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    const pid_t pid = crash_test_fork_child(dir, CRASH_CHILD_SEGV);
    MESH_TEST_FAIL_IF_CLEANUP(pid < 0, mesh_test_remove_tree(dir), "fork failed");

    int status = 0;
    MESH_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, mesh_test_remove_tree(dir),
                              "waitpid failed");

    /*
     * The child died *of the signal*, rather than exiting tidily from inside the handler.
     *
     * This is the half that is easy to lose. A handler that swallowed the fault would leave a
     * process still running on a corrupted stack, and whatever started the client - the launcher
     * on a Brick, a shell here - would be told it exited normally. The re-raise at the end of
     * the handler is what keeps that honest, and this is the only place it is checked.
     */
    MESH_TEST_FAIL_IF_CLEANUP(!WIFSIGNALED(status), mesh_test_remove_tree(dir),
                              "the child did not die of its signal");
    MESH_TEST_FAIL_IF_CLEANUP(WTERMSIG(status) != SIGSEGV, mesh_test_remove_tree(dir),
                              "the child died of the wrong signal");

    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, MESH_CRASH_REPORT_NAME);
    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(!read, "no readable report was left behind");

    MESH_TEST_FAIL_IF(strstr(body, "SIGSEGV") == NULL, "the report does not name the signal");
    MESH_TEST_FAIL_IF(strstr(body, "child-build") == NULL, "the notes were lost");
    MESH_TEST_FAIL_IF(strstr(body, "the last thing the child did") == NULL,
                      "the log ring was lost");
    /* The file is finished rather than cut off half way, which is what says the handler ran to
       the end instead of faulting inside itself. */
    MESH_TEST_FAIL_IF(strstr(body, "--- end") == NULL, "the report stops before its end marker");
    record_success(test_name);
}

MESH_TEST_CASE(crash_handler_walks_more_than_one_frame, unit) {
    /*
     * Its own case rather than another assertion on the one above, because it is the part most
     * likely to quietly stop working: the walk is guarded so heavily that every way of getting
     * it wrong ends in *fewer frames*, never in a crash or an error. It shipped once requiring
     * sixteen-byte-aligned frame pointers, which is true on aarch64 and false on x86-64, and the
     * symptom was a report with a single plausible address in it - indistinguishable from a
     * genuinely shallow stack unless something counts.
     *
     * Four calls deep plus the handler's own frames, so three is a floor a working walk clears
     * easily and a broken one cannot reach.
     */
    char dir[] = "/tmp/mesh_crash_walkXXXXXX";
    MESH_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    const pid_t pid = crash_test_fork_child(dir, CRASH_CHILD_SEGV);
    MESH_TEST_FAIL_IF_CLEANUP(pid < 0, mesh_test_remove_tree(dir), "fork failed");
    int status = 0;
    MESH_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, mesh_test_remove_tree(dir),
                              "waitpid failed");

    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, MESH_CRASH_REPORT_NAME);
    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(!read, "no readable report was left behind");

    const char *stack = strstr(body, "--- stack");
    MESH_TEST_FAIL_IF(stack == NULL, "the report has no stack section");
    unsigned frames = 0U;
    for (const char *at = strstr(stack, " #"); at != NULL; at = strstr(at + 2, " #")) {
        ++frames;
    }
    MESH_TEST_FAIL_IF(frames < 3U, "the stack walk stopped after one or two frames");
    record_success(test_name);
}

MESH_TEST_CASE(crash_handler_catches_an_abort, unit) {
    /* SIGABRT is the one in the set that is not a fault: an assert, or libc finding something it
       refuses to continue past. It is worth the entry because it is how most *deliberate*
       stops arrive, and a set that caught only the four faults would miss all of them. */
    char dir[] = "/tmp/mesh_crash_abortXXXXXX";
    MESH_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    const pid_t pid = crash_test_fork_child(dir, CRASH_CHILD_ABORT);
    MESH_TEST_FAIL_IF_CLEANUP(pid < 0, mesh_test_remove_tree(dir), "fork failed");
    int status = 0;
    MESH_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, mesh_test_remove_tree(dir),
                              "waitpid failed");
    MESH_TEST_FAIL_IF_CLEANUP(!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT,
                              mesh_test_remove_tree(dir), "the child did not die of SIGABRT");

    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, MESH_CRASH_REPORT_NAME);
    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(!read, "an abort left no report");
    MESH_TEST_FAIL_IF(strstr(body, "SIGABRT") == NULL, "the report does not name the signal");
    record_success(test_name);
}

/* ---- what the client is told afterwards --------------------------------------------------------
 */

MESH_TEST_CASE(crash_report_waiting_is_read_once_at_install, unit) {
    /*
     * The rule the About section and the banner both rest on: a client learns at startup whether
     * the *previous* run crashed, and never changes its mind afterwards.
     *
     * Asked on demand instead, the flag would flip the moment this run wrote its own report -
     * so a client would start telling the user it had crashed while they were still using it,
     * and the banner would appear underneath a fault that had not finished happening.
     */
    char dir[] = "/tmp/mesh_crash_waitXXXXXX";
    MESH_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    /* Nothing there yet, so a fresh process says no. */
    pid_t pid = crash_test_fork_child(dir, CRASH_CHILD_CLEAN);
    MESH_TEST_FAIL_IF_CLEANUP(pid < 0, mesh_test_remove_tree(dir), "fork failed");
    int status = 0;
    MESH_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, mesh_test_remove_tree(dir),
                              "waitpid failed");
    MESH_TEST_FAIL_IF_CLEANUP(!WIFEXITED(status) || WEXITSTATUS(status) != 0,
                              mesh_test_remove_tree(dir),
                              "a directory with no report reported one waiting");

    /* Now crash one, and a *later* process finds it. */
    pid = crash_test_fork_child(dir, CRASH_CHILD_SEGV);
    MESH_TEST_FAIL_IF_CLEANUP(pid < 0, mesh_test_remove_tree(dir), "fork failed");
    MESH_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, mesh_test_remove_tree(dir),
                              "waitpid failed");

    pid = crash_test_fork_child(dir, CRASH_CHILD_CLEAN);
    MESH_TEST_FAIL_IF_CLEANUP(pid < 0, mesh_test_remove_tree(dir), "fork failed");
    MESH_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, mesh_test_remove_tree(dir),
                              "waitpid failed");
    MESH_TEST_FAIL_IF_CLEANUP(!WIFEXITED(status) || WEXITSTATUS(status) != 1,
                              mesh_test_remove_tree(dir),
                              "the report left by a previous run was not noticed");

    mesh_test_remove_tree(dir);
    record_success(test_name);
}

/*
 * Discarding, from a child for the reason every other install here runs in one - and checked
 * from the parent, which can still see the directory after the child is gone.
 *
 * The exit codes are the assertions: 0 only if the file was there, the discard reported success,
 * the flag went down, and a second discard was also fine. That last one matters because the
 * banner's resolution is a press, and a press the user makes twice must not turn into an error
 * the second time.
 */
static pid_t crash_test_fork_discard(const char *dir) {
    const pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    if (mesh_crash_install(dir) != 0) {
        _exit(40);
    }
    if (!mesh_crash_report_waiting()) {
        _exit(41);
    }
    if (mesh_crash_discard() != 0) {
        _exit(42);
    }
    if (mesh_crash_report_waiting()) {
        _exit(43);
    }
    /* Again, on nothing. */
    if (mesh_crash_discard() != 0) {
        _exit(44);
    }
    /* The path is still answerable with no report on disk: it is where one *would* go, which is
       what lets the About row say so before anything has gone wrong. */
    char path[MESH_CRASH_PATH_MAX];
    if (!mesh_crash_report_path(path, sizeof path) || path[0] == '\0') {
        _exit(45);
    }
    _exit(0);
}

MESH_TEST_CASE(crash_discard_removes_the_report_and_repeats_cleanly, unit) {
    char dir[] = "/tmp/mesh_crash_discardXXXXXX";
    MESH_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    pid_t pid = crash_test_fork_child(dir, CRASH_CHILD_SEGV);
    MESH_TEST_FAIL_IF_CLEANUP(pid < 0, mesh_test_remove_tree(dir), "fork failed");
    int status = 0;
    MESH_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, mesh_test_remove_tree(dir),
                              "waitpid failed");

    pid = crash_test_fork_discard(dir);
    MESH_TEST_FAIL_IF_CLEANUP(pid < 0, mesh_test_remove_tree(dir), "fork failed");
    MESH_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, mesh_test_remove_tree(dir),
                              "waitpid failed");
    MESH_TEST_FAIL_IF_CLEANUP(!WIFEXITED(status), mesh_test_remove_tree(dir),
                              "the discarding child died");

    const int code = WEXITSTATUS(status);
    MESH_TEST_FAIL_IF_CLEANUP(code == 41, mesh_test_remove_tree(dir),
                              "the report was not seen as waiting");
    MESH_TEST_FAIL_IF_CLEANUP(code == 42, mesh_test_remove_tree(dir), "the discard failed");
    MESH_TEST_FAIL_IF_CLEANUP(code == 43, mesh_test_remove_tree(dir),
                              "the flag stayed up after a discard");
    MESH_TEST_FAIL_IF_CLEANUP(code == 44, mesh_test_remove_tree(dir),
                              "a second discard reported a failure");
    MESH_TEST_FAIL_IF_CLEANUP(code == 45, mesh_test_remove_tree(dir),
                              "the path stopped being answerable once the report was gone");
    MESH_TEST_FAIL_IF_CLEANUP(code != 0, mesh_test_remove_tree(dir), "the discarding child failed");

    /* And from out here: the file really is gone from the filesystem, not merely from the
       module's opinion of it. */
    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, MESH_CRASH_REPORT_NAME);
    struct stat info;
    const bool still_there = stat(path, &info) == 0;
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(still_there, "the report is still on disk after a discard");
    record_success(test_name);
}

MESH_TEST_CASE(crash_install_re_aims_rather_than_ignoring_a_second_call, unit) {
    /*
     * A second install points the report at the new directory.
     *
     * This is here because the opposite - returning early once installed - is the obvious
     * implementation and hides a real bug: the signal disposition is genuinely once per process,
     * but the *path* is not, so a caller passing a different directory got success and the old
     * location. It surfaced as every forked case in this file inheriting an install from
     * whichever suite ran first and writing its report somewhere nobody was looking, which is
     * precisely the shape of failure a caller never thinks to check for.
     */
    char first[] = "/tmp/mesh_crash_aim1XXXXXX";
    char second[] = "/tmp/mesh_crash_aim2XXXXXX";
    MESH_TEST_FAIL_IF(!crash_test_tempdir(first), "mkdtemp failed");
    MESH_TEST_FAIL_IF_CLEANUP(!crash_test_tempdir(second), mesh_test_remove_tree(first),
                              "mkdtemp failed");

    const pid_t pid = fork();
    MESH_TEST_FAIL_IF_CLEANUP(pid < 0, mesh_test_remove_tree(first);
                              mesh_test_remove_tree(second), "fork failed");
    if (pid == 0) {
        if (mesh_crash_install(first) != 0) {
            _exit(40);
        }
        if (mesh_crash_install(second) != 0) {
            _exit(41);
        }
        char path[MESH_CRASH_PATH_MAX];
        if (!mesh_crash_report_path(path, sizeof path)) {
            _exit(42);
        }
        _exit(strstr(path, second) != NULL ? 0 : 43);
    }
    int status = 0;
    MESH_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, mesh_test_remove_tree(first);
                              mesh_test_remove_tree(second), "waitpid failed");
    const bool re_aimed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    mesh_test_remove_tree(first);
    mesh_test_remove_tree(second);
    MESH_TEST_FAIL_IF(!re_aimed, "a second install did not re-aim the report");
    record_success(test_name);
}

MESH_TEST_CASE(crash_install_refuses_what_it_cannot_name, unit) {
    /* A directory whose name leaves no room for the report's own is refused rather than
       truncated: a path cut short is a path that names some other file. */
    char dir[MESH_CRASH_PATH_MAX + 64U];
    memset(dir, 'a', sizeof dir - 1U);
    dir[0] = '/';
    dir[sizeof dir - 1U] = '\0';

    const pid_t pid = fork();
    MESH_TEST_FAIL_IF(pid < 0, "fork failed");
    if (pid == 0) {
        _exit(mesh_crash_install(dir) == 0 ? 1 : 0);
    }
    int status = 0;
    MESH_TEST_FAIL_IF(waitpid(pid, &status, 0) != pid, "waitpid failed");
    MESH_TEST_FAIL_IF(!WIFEXITED(status) || WEXITSTATUS(status) != 0,
                      "an over-long directory was accepted");

    /* An empty one is the other end of the same question. */
    MESH_TEST_FAIL_IF(mesh_crash_install("") == 0, "an empty directory was accepted");
    MESH_TEST_FAIL_IF(mesh_crash_install(NULL) == 0, "a NULL directory was accepted");
    record_success(test_name);
}
