#ifndef MESH_UTILS_CRASH_H
#define MESH_UTILS_CRASH_H

/*
 * What the client leaves behind when it faults.
 *
 * Until this existed a SIGSEGV took the process down mid-sentence: `launch.sh` pipes the client
 * through `tee`, so the log on the SD card simply stopped, with nothing in it saying the process
 * had died or where. A report from a stranger was therefore "it crashed", and there was nothing
 * to ask them for - the one fact worth having was the one the fault destroyed.
 *
 * So this writes a file. Not a service, and deliberately nothing that leaves the device: the
 * memory of this process holds node names, every positioned node's coordinates, the message log
 * and the channel keys, so a dump of it is the last thing that should go anywhere by itself.
 * What lands on disk is a page of text the user can read before they decide to share it.
 *
 * ---- the discipline -------------------------------------------------------------------------
 *
 * A handler runs on a process that is already broken, which rules out most of libc. POSIX names
 * the functions that stay safe there and neither `printf` nor `malloc` is among them - a fault
 * inside `malloc` leaves the allocator's lock held, and a handler that takes it deadlocks
 * instead of reporting anything. So the writer here uses `write()` and formats its own integers,
 * and **every string it might need is built at install time**, in ordinary context, where
 * `snprintf` is allowed: the report's path, the executable's load address, the fixed headings.
 * The handler assembles nothing it did not already have.
 *
 * The one thing it must do that cannot be prepared is read the crashed stack, and that is why
 * `mesh_crash_install()` makes a pipe it never sends anything through. Probing an address by
 * writing it to a descriptor turns an unreadable page into `EFAULT` - a return value - where
 * dereferencing it would be a second fault inside the handler. It is what lets the backtrace be
 * attempted at all rather than being left out as too dangerous.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The report's name inside the data directory. Fixed rather than stamped: see
   mesh_crash_install() for why the newest crash is allowed to replace an older one. */
#define MESH_CRASH_REPORT_NAME "crash.txt"

/* Enough for the data directory plus the name above. The same order of magnitude as
   MESH_FETCH_PATH_MAX next door, and for the same reason - a path here is a value, not an
   allocation. */
#define MESH_CRASH_PATH_MAX 256U

/* How much of the log the report carries, and how much of a line survives. 32 lines is about
   what a reader needs to see the last thing the client did; a line longer than this is
   invariably a dump of something rather than a sentence, and is cut with a marker. */
#define MESH_CRASH_LOG_LINES 32U
#define MESH_CRASH_LOG_LINE_MAX 160U

/* The longest a note's value may be. A route name, a version, a transport state - all short by
   construction, and a note is a fact about the client rather than anything off the air. */
#define MESH_CRASH_NOTE_MAX 96U

/*
 * The facts the report carries about where the client was standing.
 *
 * A fixed set of slots rather than a map of names, which is the same choice `status.c` makes
 * about verbs: a table that can be read in one place beats one assembled at runtime, and the
 * handler must not be looking anything up. Each is set from ordinary context whenever the
 * client's own state changes, and the handler only ever writes out what it finds.
 */
enum mesh_crash_note_slot {
    /* Which build this is, so a report names the binary it came from. */
    MESH_CRASH_NOTE_VERSION = 0,
    /* Where the UI was: the route, as `mesh_ui_route_describe()` spells it. The single most
       useful line in the file, because it turns "it crashed" into "it crashed on the map". */
    MESH_CRASH_NOTE_ROUTE,
    /* What the link was doing - the transport's own status string. The other half of the same
       question, since most of what this client does at all is driven by a radio. */
    MESH_CRASH_NOTE_TRANSPORT,
    MESH_CRASH_NOTE_SLOT_COUNT,
};

/*
 * Install the handlers and decide where a report would go.
 *
 * `dir` is the data directory ($HOME/.meshclient); the report is that plus
 * MESH_CRASH_REPORT_NAME. Returns 0, or -errno when the handlers could not be installed - in
 * which case nothing else here does anything, which is the right failure: a client that cannot
 * report a crash is still a client.
 *
 * Calling it twice is not an error and does not stack handlers: the signal dispositions are set
 * once, and a later call re-aims the report at the directory it was given rather than quietly
 * keeping the first one. That distinction matters because the path is not the disposition - a
 * function that ignored its own argument and reported success is the failure this avoids.
 *
 * **A report left from a previous run is read here and kept.** `mesh_crash_report_waiting()`
 * answers from what was on disk at this moment rather than from a `stat` on demand, so a report
 * written by *this* run's own fault cannot make the running client claim it has already
 * crashed - which is a screen contradicting itself, and would be the ordinary case for anybody
 * looking at the About section while something went wrong underneath them.
 *
 * The path is fixed, so a second crash overwrites the first. That is deliberate: the reader has
 * just watched the client die, and a file describing a fault from last week while the one they
 * are holding is thrown away would be the wrong half kept.
 */
int mesh_crash_install(const char *dir);

/* Where a report would be written, whether or not one is there. False when no install has
   succeeded, leaving `out` an empty string. */
bool mesh_crash_report_path(char *out, size_t out_len);

/* Whether a report from a previous run was waiting when mesh_crash_install() ran. */
bool mesh_crash_report_waiting(void);

/*
 * Remove the waiting report and stop saying there is one.
 *
 * Returns 0 when the file is gone - including when it was gone already, since a discard that
 * runs twice is not a failure. This is what makes the banner resolvable: there is somewhere for
 * the notice to go when it is pressed.
 */
int mesh_crash_discard(void);

/*
 * Record a fact for the next report. `value` is copied; NULL or an empty string clears the slot.
 *
 * Cheap enough to call on every publish - it is a bounded copy into a static buffer - which is
 * what keeps the route note honest without anything having to decide when to refresh it.
 */
void mesh_crash_note(enum mesh_crash_note_slot slot, const char *value);

/*
 * Put one already-formatted log line into the ring the report will carry.
 *
 * Called by `mesh_log_message_v()` and by nothing else. It is here rather than in log.c because
 * the ring is the crash report's, and the dependency has to run this way round: a handler that
 * called into the logger would be a handler using stdio on a broken process, which is the one
 * thing this module exists to avoid.
 *
 * Safe before any install: the ring is static and filling it costs a copy, so a fault during
 * startup still has the lines that led to it.
 */
void mesh_crash_log_line(const char *line);

/*
 * Write a report for `signal_number` to `fd` exactly as the handler would, minus the fault
 * registers there is no fault to read.
 *
 * Exists for the tests, which is worth stating plainly: the handler proper cannot be called
 * directly without raising a real signal, and a suite that raised SIGSEGV in-process would be a
 * suite betting on its own handler. This is the same writer with the arch-specific half left
 * out, so the formatting, the notes and the log ring are all checked by ordinary means, and
 * only the register read and the stack walk are left to the forked case.
 */
void mesh_crash_write_report(int fd, int signal_number);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UTILS_CRASH_H */
