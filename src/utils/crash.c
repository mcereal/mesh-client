/*
 * The crash handler. Read include/mesh/utils/crash.h first - the rules this file follows are
 * stated there, and most of what looks roundabout below is one of them.
 *
 * _GNU_SOURCE rather than _POSIX_C_SOURCE, which is what the rest of src/utils asks for: the
 * register names on x86-64 (REG_RIP) and pipe2() are both behind it, and musl and glibc agree
 * about that much.
 */
#define _GNU_SOURCE

#include "mesh/utils/crash.h"

#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

/* ---- what the handler is allowed to have ----------------------------------------------------
 *
 * Everything in this block is written from ordinary context and only ever read from the signal
 * handler. `sig_atomic_t` where the handler and the rest of the client both touch it; plain
 * arrays where the handler only reads and a torn read costs a garbled line rather than a fault.
 */

/* The report's path, built once by mesh_crash_install(). The handler opens this and nothing
   else, because building a path is string work and string work is what it may not do. */
static char g_report_path[MESH_CRASH_PATH_MAX];
static bool g_installed;
static bool g_report_waiting;

/* Where this image is mapped, already formatted as "0x...". Read out of /proc/self/maps at
   install time, because the whole point of the number is to be subtracted from the addresses
   below and a PIE build's are meaningless without it. Empty when it could not be read, in which
   case the report says so rather than printing a zero that looks like an answer. */
static char g_load_base[32];

/* The probe pipe. Writing an address to it reports EFAULT instead of faulting, which is what
   makes walking a broken stack survivable. Both ends non-blocking so a probe can never wait. */
static int g_probe_fd[2] = {-1, -1};

/*
 * One byte longer than anything that is ever written into them, and the spare byte is never
 * touched after startup.
 *
 * A note is copied in from ordinary context - byte by byte, with the NUL written last - while a
 * fault may land at any point in the middle of it. For the window between the first byte and
 * that NUL the buffer holds no terminator at all, so the handler's strlen() would run off the
 * end of the array and print whatever static happened to follow. It is a narrow window and the
 * consequence is only a garbled line, but a crash handler is the last place to leave a read
 * that is unbounded in principle, and a permanent zero at the end costs three bytes.
 */
static char g_notes[MESH_CRASH_NOTE_SLOT_COUNT][MESH_CRASH_NOTE_MAX + 1U];

static const char *const k_note_labels[MESH_CRASH_NOTE_SLOT_COUNT] = {
    [MESH_CRASH_NOTE_VERSION] = "version      ",
    [MESH_CRASH_NOTE_ROUTE] = "route        ",
    [MESH_CRASH_NOTE_TRANSPORT] = "transport    ",
};

/*
 * The log ring.
 *
 * `g_log_next` is where the next line goes, so the oldest line is the one it is pointing at once
 * the ring has wrapped. It is `sig_atomic_t` because the handler reads it while ordinary code
 * may be part way through a line: the worst that costs is one torn line at the end of the
 * report, which is also the line that matters least - if the client was mid-log when it faulted
 * the fault is what happened next.
 */
static char g_log[MESH_CRASH_LOG_LINES][MESH_CRASH_LOG_LINE_MAX + 1U];
static volatile sig_atomic_t g_log_next;
static volatile sig_atomic_t g_log_filled;

/* The signals worth catching: the four faults and the abort that an assert or a libc check
   raises. SIGQUIT and the rest are ways of being asked to stop, which is not a crash. */
static const int k_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

/* ---- writing without libc -------------------------------------------------------------------- */

/* write(2) until it is done or refuses. A short write on a regular file is possible (a full
   card, a signal) and a report that stopped half way is still worth having, so a refusal ends
   the line rather than the report. */
static void crash_write(int fd, const char *data, size_t len) {
    while (len > 0U) {
        const ssize_t written = write(fd, data, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (written == 0) {
            return;
        }
        data += (size_t)written;
        len -= (size_t)written;
    }
}

static void crash_puts(int fd, const char *text) {
    if (text != NULL) {
        crash_write(fd, text, strlen(text));
    }
}

/*
 * An unsigned integer, in `base`, into `out`. Returns the length.
 *
 * Here because snprintf is not on POSIX's async-signal-safe list, and the reason it is not is
 * exactly the reason it matters here: a locale-aware formatter takes locks. `out` is written
 * backwards and reversed, which is the shortest correct way to do this without a division table.
 */
static size_t crash_format_unsigned(char *out, size_t out_len, uint64_t value, unsigned base,
                                    size_t pad) {
    static const char k_digits[] = "0123456789abcdef";
    char scratch[32];
    size_t len = 0U;

    do {
        scratch[len++] = k_digits[value % base];
        value /= base;
    } while (value != 0U && len < sizeof scratch);
    while (len < pad && len < sizeof scratch) {
        scratch[len++] = '0';
    }

    if (len >= out_len) {
        len = out_len > 0U ? out_len - 1U : 0U;
    }
    for (size_t i = 0U; i < len; ++i) {
        out[i] = scratch[len - 1U - i];
    }
    if (out_len > 0U) {
        out[len] = '\0';
    }
    return len;
}

static void crash_write_unsigned(int fd, uint64_t value) {
    char buffer[32];
    const size_t len = crash_format_unsigned(buffer, sizeof buffer, value, 10U, 0U);
    crash_write(fd, buffer, len);
}

/* An address, as a fixed sixteen digits so a column of them lines up and a short one is
   obviously an address rather than a count. */
static void crash_write_address(int fd, uint64_t value) {
    char buffer[32];
    crash_puts(fd, "0x");
    const size_t len = crash_format_unsigned(buffer, sizeof buffer, value, 16U, 16U);
    crash_write(fd, buffer, len);
}

/* ---- probing memory -------------------------------------------------------------------------
 *
 * The trick this module turns on. write(2) validates its buffer in the kernel and answers
 * EFAULT for a page that is not there, so asking the pipe whether an address is readable costs
 * a syscall and cannot fault - where the obvious `*(uint64_t *)fp` costs a second SIGSEGV
 * inside the handler for the first one.
 *
 * Whatever is written has to come straight back out or the pipe fills and the next probe blocks
 * - hence the drain, and hence the non-blocking ends as the belt to that brace.
 */
static bool crash_readable(const void *address, size_t len) {
    if (g_probe_fd[0] < 0 || g_probe_fd[1] < 0 || address == NULL) {
        return false;
    }
    const ssize_t written = write(g_probe_fd[1], address, len);
    if (written < 0) {
        return false;
    }
    char sink[64];
    size_t drained = 0U;
    while (drained < (size_t)written) {
        const ssize_t got = read(g_probe_fd[0], sink, sizeof sink);
        if (got <= 0) {
            break;
        }
        drained += (size_t)got;
    }
    return (size_t)written == len;
}

/* ---- the fault itself ------------------------------------------------------------------------ */

static const char *crash_signal_name(int signal_number) {
    switch (signal_number) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    default:
        return "signal";
    }
}

/*
 * The program counter and frame pointer out of the interrupted context.
 *
 * Two architectures because those are the two this project builds for - aarch64 is the Brick and
 * x86-64 is every test run and every container build - and anything else gets a report with no
 * stack rather than a build failure. That is the right trade: the notes and the log tail are
 * most of the value, and they are portable.
 */
static bool crash_registers(void *ucontext, uint64_t *pc, uint64_t *fp) {
    if (ucontext == NULL) {
        return false;
    }
    const ucontext_t *uc = (const ucontext_t *)ucontext;
#if defined(__aarch64__)
    *pc = (uint64_t)uc->uc_mcontext.pc;
    /* x29 is the frame pointer under AAPCS64. */
    *fp = (uint64_t)uc->uc_mcontext.regs[29];
    return true;
#elif defined(__x86_64__)
    *pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    *fp = (uint64_t)uc->uc_mcontext.gregs[REG_RBP];
    return true;
#else
    (void)uc;
    (void)pc;
    (void)fp;
    return false;
#endif
}

/*
 * Walk the frame pointers.
 *
 * Both supported architectures lay a frame out the same way once a frame pointer is kept: the
 * saved pointer to the caller's frame first, the return address next. The chain is trusted only
 * as far as it stays plausible - readable, aligned, and climbing - because a corrupted stack is
 * the ordinary case here rather than the exception, and a walk that believed it would print
 * whatever happened to be in memory as though it were a call chain.
 *
 * What makes the whole thing safe is the probe rather than the checks: the checks stop nonsense
 * being printed, the probe stops the handler dying.
 */
static void crash_write_frames(int fd, uint64_t fp) {
    uint64_t previous = 0U;
    for (unsigned depth = 0U; depth < 32U; ++depth) {
        /*
         * Eight, not sixteen. AAPCS64 really does keep the stack sixteen-aligned throughout, so
         * an aarch64 x29 is always 0 mod 16 and the tighter test looks like the safer one - but
         * x86-64 only promises that alignment at a call boundary, and a frame there is as often
         * 8 mod 16 as not. Asked for sixteen, the walk ended after a single frame on every host
         * build: one plausible address, printed without complaint, where a chain was expected.
         * A frame pointer cannot be less than pointer-aligned, and that is the whole of what is
         * knowable here.
         */
        if (fp == 0U || (fp & (uint64_t)(sizeof(uint64_t) - 1U)) != 0U || fp <= previous) {
            return;
        }
        if (!crash_readable((const void *)(uintptr_t)fp, 2U * sizeof(uint64_t))) {
            return;
        }
        const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;
        const uint64_t next = frame[0];
        const uint64_t return_address = frame[1];
        if (return_address == 0U) {
            return;
        }
        crash_puts(fd, " #");
        char index[8];
        const size_t len = crash_format_unsigned(index, sizeof index, depth, 10U, 2U);
        crash_write(fd, index, len);
        crash_puts(fd, " ");
        crash_write_address(fd, return_address);
        crash_puts(fd, "\n");
        previous = fp;
        fp = next;
    }
}

/* ---- the report ------------------------------------------------------------------------------ */

static void crash_write_notes(int fd) {
    for (unsigned slot = 0U; slot < (unsigned)MESH_CRASH_NOTE_SLOT_COUNT; ++slot) {
        if (g_notes[slot][0] == '\0') {
            continue;
        }
        crash_puts(fd, k_note_labels[slot]);
        crash_puts(fd, g_notes[slot]);
        crash_puts(fd, "\n");
    }
}

/*
 * The log tail, oldest first.
 *
 * `g_log_filled` is what tells a ring that has wrapped from one that has not, which decides
 * where "oldest" is. Reading both while the writer may be running is why a line can be torn; see
 * the declaration for why that is the right thing to accept.
 */
static void crash_write_log(int fd) {
    const unsigned filled = (unsigned)g_log_filled;
    const unsigned next = (unsigned)g_log_next;
    if (filled == 0U) {
        crash_puts(fd, "(the client logged nothing before it stopped)\n");
        return;
    }
    const unsigned count = filled < MESH_CRASH_LOG_LINES ? filled : MESH_CRASH_LOG_LINES;
    const unsigned first = filled < MESH_CRASH_LOG_LINES ? 0U : next;
    for (unsigned i = 0U; i < count; ++i) {
        const unsigned index = (first + i) % MESH_CRASH_LOG_LINES;
        crash_puts(fd, g_log[index]);
        crash_puts(fd, "\n");
    }
}

/*
 * The whole file, in the order it has to be written.
 *
 * The ordering is a safety property rather than a matter of taste. Everything that cannot fault
 * - the headings, the notes, the log - goes down first, and the stack walk goes last, so a
 * handler that dies part way through has already put the useful half on disk. write() has handed
 * the data to the kernel by the time it returns, so a process killed a moment later does not
 * take those pages with it.
 */
static void crash_report(int fd, int signal_number, const siginfo_t *info, void *ucontext) {
    crash_puts(fd,
               "MeshClient crash report\n"
               "=======================\n"
               "\n"
               "MeshClient wrote this file when it stopped unexpectedly. It carries no message\n"
               "text, no node names, no coordinates and no channel keys - only the lines below.\n"
               "Nothing was sent anywhere; it is yours to read, and to attach to a bug report at\n"
               "https://github.com/mcereal/mesh-client/issues if you would like it fixed.\n"
               "\n");

    crash_puts(fd, "signal       ");
    crash_write_unsigned(fd, (uint64_t)signal_number);
    crash_puts(fd, " (");
    crash_puts(fd, crash_signal_name(signal_number));
    crash_puts(fd, ")\n");

    if (info != NULL) {
        crash_puts(fd, "code         ");
        crash_write_unsigned(fd, (uint64_t)info->si_code);
        crash_puts(fd, "\nfault addr   ");
        crash_write_address(fd, (uint64_t)(uintptr_t)info->si_addr);
        crash_puts(fd, "\n");
    }

    /* Monotonic, so it is how long this run lasted rather than what the clock claims. On a Brick
       it is the only one of the two that means anything: there is no RTC battery, so a device
       that has not reached a network boots into 1970 and says so below. */
    crash_puts(fd, "uptime ms    ");
    crash_write_unsigned(fd, (uint64_t)mesh_time_monotonic_ms());
    crash_puts(fd, "\nwall clock   ");
    crash_write_unsigned(fd, (uint64_t)time(NULL));
    crash_puts(fd, " (seconds since 1970; a device with no RTC reads small here)\n");

    if (g_load_base[0] != '\0') {
        crash_puts(fd, "load base    ");
        crash_puts(fd, g_load_base);
        crash_puts(fd, "\n");
    }
    crash_write_notes(fd);

    uint64_t pc = 0U;
    uint64_t fp = 0U;
    const bool have_registers = crash_registers(ucontext, &pc, &fp);

    crash_puts(fd, "\n--- where ---------------------------------------------------------------\n");
    if (have_registers) {
        crash_puts(fd, "pc           ");
        crash_write_address(fd, pc);
        crash_puts(fd, "\n");
    } else {
        crash_puts(fd, "(no registers for this build's architecture)\n");
    }
    crash_puts(fd, "Resolve an address with:  addr2line -fpe meshclient <address - load base>\n");

    crash_puts(fd, "\n--- log -----------------------------------------------------------------\n");
    crash_write_log(fd);

    /*
     * Last, and alone down here, because it is the only part that reads memory the fault has
     * already proved untrustworthy.
     */
    if (have_registers) {
        crash_puts(fd,
                   "\n--- stack ---------------------------------------------------------------\n");
        crash_write_frames(fd, fp);
    }
    crash_puts(fd, "\n--- end -----------------------------------------------------------------\n");
}

void mesh_crash_write_report(int fd, int signal_number) {
    crash_report(fd, signal_number, NULL, NULL);
}

/* ---- the handler ----------------------------------------------------------------------------- */

static void crash_handler(int signal_number, siginfo_t *info, void *ucontext) {
    /*
     * Stand every handler down before doing anything else.
     *
     * Two things come out of it. A fault *inside* this function then kills the process the
     * ordinary way instead of re-entering here forever, which is the failure mode a crash
     * handler is most likely to have. And the re-raise at the bottom reaches the default
     * disposition, so the process dies of the signal it was actually given - which is what keeps
     * the exit status honest for whatever started us.
     */
    for (size_t i = 0U; i < sizeof k_signals / sizeof k_signals[0]; ++i) {
        signal(k_signals[i], SIG_DFL);
    }

    if (g_report_path[0] != '\0') {
        const int fd = open(g_report_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            crash_report(fd, signal_number, info, ucontext);
            /* The Brick's card is mounted `sync`, so this is close to free there - and on
               anything else it is what makes the report survive a device that loses power
               between the fault and the next clean unmount. */
            (void)fsync(fd);
            (void)close(fd);
        }
    }

    raise(signal_number);
}

/* ---- install --------------------------------------------------------------------------------- */

/*
 * The first mapping of this process, as text.
 *
 * /proc/self/maps opens with the executable's own first segment, so its start address is the
 * load base every address in the report has to be measured from. Read here, in ordinary context,
 * because parsing is exactly what the handler may not do - by the time it runs this is a string
 * to be written out and nothing more.
 */
static void crash_capture_load_base(void) {
    g_load_base[0] = '\0';
    FILE *maps = fopen("/proc/self/maps", "re");
    if (maps == NULL) {
        return;
    }
    char line[256];
    if (fgets(line, sizeof line, maps) != NULL) {
        /*
         * The field is the hex digits up to the first dash, and it is checked rather than
         * trusted. An address is sixteen digits at the very most, so anything longer - or
         * carrying anything that is not a digit - is a /proc that does not look like the one
         * this expects, and an unset base prints an honest "no base" where a copied prefix
         * would print a number the reader would go on to subtract.
         */
        size_t len = 0U;
        while (len < sizeof g_load_base - 3U && line[len] != '\0' && line[len] != '-') {
            const char c = line[len];
            const bool hex =
                (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex) {
                len = 0U;
                break;
            }
            ++len;
        }
        if (len > 0U && line[len] == '-') {
            g_load_base[0] = '0';
            g_load_base[1] = 'x';
            memcpy(g_load_base + 2, line, len);
            g_load_base[len + 2U] = '\0';
        }
    }
    (void)fclose(maps);
}

int mesh_crash_install(const char *dir) {
    if (dir == NULL || dir[0] == '\0') {
        return -EINVAL;
    }

    /*
     * Re-entering this re-aims the report and leaves the handlers alone.
     *
     * The obvious shape - return early once installed - looked right and was wrong in a way
     * worth recording. A signal disposition really is once per process, so *installing* twice is
     * a mistake; but the path is not the disposition, and a second call carrying a different
     * directory then kept the first one silently, which is a function that ignores its own
     * argument and says it succeeded. It showed up first in a forked test inheriting an install
     * from its parent, which is exactly the case a caller would never think to check.
     *
     * `g_report_waiting` is re-read here for the same reason. The rule it exists for is that a
     * client does not learn *mid-run* that it has crashed; an install is the defined moment for
     * asking, and a second install is another one.
     */
    const int written =
        snprintf(g_report_path, sizeof g_report_path, "%s/%s", dir, MESH_CRASH_REPORT_NAME);
    if (written < 0 || (size_t)written >= sizeof g_report_path) {
        g_report_path[0] = '\0';
        return -ENAMETOOLONG;
    }

    /*
     * Answered once, here, rather than by a stat() whenever somebody asks.
     *
     * The difference shows the moment this run writes its own report: a client that looked at
     * the disk on demand would start telling the user it had already crashed while they were
     * still using it, on a screen that is meant to be reporting the *previous* run.
     */
    g_report_waiting = access(g_report_path, F_OK) == 0;

    /* The probe pipe and the load base are properties of the process rather than of the
       directory, so they are taken once however many times this is called - otherwise a second
       install would leak a pair of descriptors for nothing. */
    if (g_probe_fd[0] < 0 && pipe2(g_probe_fd, O_CLOEXEC | O_NONBLOCK) != 0) {
        /* No probe means no safe stack walk. Everything else in a report still works, so this
           is not a reason to refuse the install - crash_readable() simply always says no. */
        g_probe_fd[0] = -1;
        g_probe_fd[1] = -1;
    }
    if (!g_installed) {
        crash_capture_load_base();
    }

    if (g_installed) {
        return 0;
    }

    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    /* Block the other fault signals while one is being reported, so two arriving together are
       one report rather than two interleaved into the same file. */
    sigemptyset(&action.sa_mask);
    for (size_t i = 0U; i < sizeof k_signals / sizeof k_signals[0]; ++i) {
        sigaddset(&action.sa_mask, k_signals[i]);
    }

    for (size_t i = 0U; i < sizeof k_signals / sizeof k_signals[0]; ++i) {
        if (sigaction(k_signals[i], &action, NULL) != 0) {
            return -errno;
        }
    }

    g_installed = true;
    return 0;
}

bool mesh_crash_report_path(char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';
    if (g_report_path[0] == '\0') {
        return false;
    }
    mesh_str_copy(out, out_len, g_report_path);
    return true;
}

bool mesh_crash_report_waiting(void) { return g_report_waiting; }

int mesh_crash_discard(void) {
    if (g_report_path[0] == '\0') {
        return -EINVAL;
    }
    if (unlink(g_report_path) != 0 && errno != ENOENT) {
        return -errno;
    }
    g_report_waiting = false;
    return 0;
}

void mesh_crash_note(enum mesh_crash_note_slot slot, const char *value) {
    if ((unsigned)slot >= (unsigned)MESH_CRASH_NOTE_SLOT_COUNT) {
        return;
    }
    if (value == NULL) {
        g_notes[slot][0] = '\0';
        return;
    }
    /* MESH_CRASH_NOTE_MAX rather than sizeof, so the sentinel byte at the end stays zero. */
    mesh_str_copy(g_notes[slot], MESH_CRASH_NOTE_MAX, value);
}

void mesh_crash_log_line(const char *line) {
    if (line == NULL || line[0] == '\0') {
        return;
    }
    const unsigned index = (unsigned)g_log_next;
    /* MESH_CRASH_LOG_LINE_MAX rather than sizeof, for the sentinel; see g_notes above. */
    mesh_str_copy(g_log[index], MESH_CRASH_LOG_LINE_MAX, line);
    g_log_next = (sig_atomic_t)((index + 1U) % MESH_CRASH_LOG_LINES);
    if ((unsigned)g_log_filled < MESH_CRASH_LOG_LINES) {
        g_log_filled = (sig_atomic_t)((unsigned)g_log_filled + 1U);
    }
}
