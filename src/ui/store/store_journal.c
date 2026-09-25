#define _POSIX_C_SOURCE 200809L

/*
 * The journal's mechanics. What it is for and the four decisions behind it are in the header.
 *
 * Nothing here says which kernel it runs on: the directory is made and a temporary published
 * through inkwell's base/file.h, a size is read with fseek()/ftell() on the stream the append
 * holds, and a file is removed with remove(). The one POSIX interface is the directory listing a
 * wipe walks, which every system this builds for has.
 */

#include "mesh/ui/store_journal.h"

#include "inkwell/base/file.h"
#include "inkwell/base/text.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The suffix a rewrite's temporary adds to the file it will replace. inkwell_record_replace()
   spells it the same way, which is what lets a wipe find both kinds. */
#define JOURNAL_TEMP_SUFFIX ".tmp"

/* ---- names ---------------------------------------------------------------------------------- */

static bool journal_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_';
}

/* A plain word: non-empty, short enough, and nothing in it that could name another directory. */
static bool journal_word(const char *word, size_t max) {
    if (word == NULL || word[0] == '\0') {
        return false;
    }
    size_t length = 0U;
    for (; word[length] != '\0'; ++length) {
        if (length + 1U >= max || !journal_word_char(word[length])) {
            return false;
        }
    }
    return true;
}

bool mesh_ui_journal_enabled(const struct mesh_ui_journal *journal) {
    return journal != NULL && journal->dir[0] != '\0';
}

int mesh_ui_journal_path(const struct mesh_ui_journal *journal, const char *subject, char *out,
                         size_t out_len) {
    if (out == NULL || out_len == 0U || !journal_word(subject, MESH_UI_JOURNAL_SUBJECT_MAX)) {
        return -EINVAL;
    }
    if (!mesh_ui_journal_enabled(journal)) {
        return -ENOENT;
    }
    const int written = snprintf(out, out_len, "%s/%s%s", journal->dir, subject, journal->suffix);
    if (written <= 0 || (size_t)written >= out_len) {
        return -ENAMETOOLONG;
    }
    return 0;
}

/* ---- opening one ---------------------------------------------------------------------------- */

int mesh_ui_journal_init(struct mesh_ui_journal *journal, const char *dir, const char *suffix,
                         uint64_t max_bytes) {
    if (journal == NULL) {
        return -EINVAL;
    }
    memset(journal, 0, sizeof *journal);
    if (dir == NULL || dir[0] == '\0' || suffix == NULL || suffix[0] != '.') {
        return -EINVAL;
    }
    if (strlen(suffix) >= sizeof journal->suffix) {
        return -ENAMETOOLONG;
    }
    if (!journal_word(suffix + 1, sizeof journal->suffix)) {
        return -EINVAL;
    }
    /* Truncation would put the files somewhere other than where the caller asked, so a directory
       that does not fit is a disabled journal rather than a shortened path. */
    if (strlen(dir) >= sizeof journal->dir) {
        return -ENAMETOOLONG;
    }
    const int made = inkwell_file_mkdir(dir);
    if (made < 0 && made != -EEXIST) {
        return made;
    }
    inkwell_str_copy(journal->dir, sizeof journal->dir, dir);
    inkwell_str_copy(journal->suffix, sizeof journal->suffix, suffix);
    journal->max_bytes = max_bytes;
    return 0;
}

bool mesh_ui_journal_exists(const struct mesh_ui_journal *journal, const char *subject) {
    char path[MESH_UI_JOURNAL_PATH_MAX];
    if (mesh_ui_journal_path(journal, subject, path, sizeof path) != 0) {
        return false;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    fclose(file);
    return true;
}

/* ---- appending ------------------------------------------------------------------------------ */

/* How long the file behind `file` is, asked of the stream. In append mode every write goes to the
   end whatever the position, so moving it there to measure costs the append nothing. */
static int journal_stream_size(FILE *file, long *out) {
    if (fseek(file, 0L, SEEK_END) != 0) {
        return -errno;
    }
    const long size = ftell(file);
    if (size < 0L) {
        return -errno;
    }
    *out = size;
    return 0;
}

int mesh_ui_journal_append(const struct mesh_ui_journal *journal, const char *subject,
                           mesh_ui_journal_append_fn write, void *context, bool *out_over_cap) {
    if (out_over_cap != NULL) {
        *out_over_cap = false;
    }
    if (write == NULL) {
        return -EINVAL;
    }
    char path[MESH_UI_JOURNAL_PATH_MAX];
    const int named = mesh_ui_journal_path(journal, subject, path, sizeof path);
    if (named == -ENOENT) {
        return 0; /* disabled: quiet */
    }
    if (named != 0) {
        return named;
    }

    FILE *file = fopen(path, "a");
    if (file == NULL) {
        return -errno;
    }
    /*
     * Whether an earlier writer already put something here, asked of what was opened. It answers
     * more precisely than the name could besides: a file left empty by an open that got no further
     * is a file with nothing to continue.
     */
    long before = 0L;
    int result = journal_stream_size(file, &before);
    if (result == 0) {
        write(file, before > 0L, context);
        result = ferror(file) ? -EIO : 0;
    }
    /* And the size the cap is measured against, off the same stream and before it is let go. */
    long after = 0L;
    if (result == 0 && fflush(file) != 0) {
        result = -errno;
    }
    if (result == 0) {
        result = journal_stream_size(file, &after);
    }
    if (fclose(file) != 0 && result == 0) {
        result = -errno;
    }
    if (result != 0) {
        return result;
    }
    if (out_over_cap != NULL && journal->max_bytes != 0U) {
        *out_over_cap = (uint64_t)after > journal->max_bytes;
    }
    return 0;
}

/* ---- reading and replacing ------------------------------------------------------------------ */

int mesh_ui_journal_read(const struct mesh_ui_journal *journal, const char *subject, char *line,
                         size_t capacity, inkwell_record_visit_fn visit, void *context) {
    if (line == NULL || visit == NULL) {
        return -EINVAL;
    }
    char path[MESH_UI_JOURNAL_PATH_MAX];
    const int named = mesh_ui_journal_path(journal, subject, path, sizeof path);
    if (named != 0) {
        return named;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -errno;
    }
    const int result = inkwell_record_read(file, line, capacity, visit, context);
    fclose(file);
    return result;
}

int mesh_ui_journal_replace(const struct mesh_ui_journal *journal, const char *subject,
                            inkwell_record_write_fn write, void *context) {
    if (write == NULL) {
        return -EINVAL;
    }
    char path[MESH_UI_JOURNAL_PATH_MAX];
    const int named = mesh_ui_journal_path(journal, subject, path, sizeof path);
    if (named == -ENOENT) {
        return 0;
    }
    if (named != 0) {
        return named;
    }
    char temp[MESH_UI_JOURNAL_PATH_MAX];
    return inkwell_record_replace(path, temp, sizeof temp, write, context, false);
}

/* ---- filtering ------------------------------------------------------------------------------ */

/*
 * The next complete line of `file` into `line`, without its newline: 1 for a line, 0 at the end,
 * or a negative errno. A line that did not fit, or that ends the file without a newline, is
 * skipped - inkwell_record_read()'s rule, so a filter sees exactly the lines a reader does.
 */
static int journal_next_line(FILE *file, char *line, size_t capacity) {
    while (fgets(line, (int)capacity, file) != NULL) {
        const size_t length = strlen(line);
        bool complete = length > 0U && line[length - 1U] == '\n';
        if (!complete && length == capacity - 1U) {
            /* The newline itself may be the first byte beyond the buffer. */
            int next = fgetc(file);
            complete = next == '\n';
            if (!complete && next != EOF) {
                while ((next = fgetc(file)) != '\n' && next != EOF) {
                }
            }
        }
        if (!complete) {
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        return 1;
    }
    return ferror(file) ? -EIO : 0;
}

int mesh_ui_journal_filter(const struct mesh_ui_journal *journal, const char *subject, char *line,
                           size_t capacity, mesh_ui_journal_filter_fn filter,
                           mesh_ui_journal_filter_end_fn end, void *context) {
    if (line == NULL || capacity < 2U || capacity > (size_t)INT32_MAX || filter == NULL ||
        end == NULL) {
        return -EINVAL;
    }
    char path[MESH_UI_JOURNAL_PATH_MAX];
    const int named = mesh_ui_journal_path(journal, subject, path, sizeof path);
    if (named == -ENOENT) {
        return 0;
    }
    if (named != 0) {
        return named;
    }

    FILE *source = fopen(path, "r");
    if (source == NULL) {
        return (errno == ENOENT) ? 0 : -errno;
    }
    char temp[MESH_UI_JOURNAL_PATH_MAX];
    const int temp_named = snprintf(temp, sizeof temp, "%s" JOURNAL_TEMP_SUFFIX, path);
    if (temp_named <= 0 || (size_t)temp_named >= sizeof temp) {
        fclose(source);
        return -ENAMETOOLONG;
    }
    FILE *out = fopen(temp, "w");
    if (out == NULL) {
        const int failed = -errno;
        fclose(source);
        return failed;
    }

    int result = 0;
    int got = 0;
    while ((got = journal_next_line(source, line, capacity)) > 0) {
        filter(context, line, out);
    }
    if (got < 0) {
        result = got;
    }
    const uint32_t dropped = end(context, out);
    if (result == 0 && ferror(out)) {
        result = -EIO;
    }
    fclose(source);
    if (fclose(out) != 0 && result == 0) {
        result = -errno;
    }
    if (result == 0 && dropped == 0U) {
        /* Nothing to say, so nothing is replaced. */
        (void)remove(temp);
        return 0;
    }
    if (result == 0) {
        result = inkwell_file_replace(temp, path);
    }
    if (result != 0) {
        (void)remove(temp);
        return result;
    }
    return dropped > (uint32_t)INT32_MAX ? INT32_MAX : (int)dropped;
}

/* ---- forgetting ----------------------------------------------------------------------------- */

int mesh_ui_journal_forget(const struct mesh_ui_journal *journal, const char *subject) {
    char path[MESH_UI_JOURNAL_PATH_MAX];
    const int named = mesh_ui_journal_path(journal, subject, path, sizeof path);
    if (named == -ENOENT) {
        return 0;
    }
    if (named != 0) {
        return named;
    }
    if (remove(path) != 0 && errno != ENOENT) {
        return -errno;
    }
    return 0;
}

/* Whether `name` ends in `suffix`, exactly. */
static bool journal_ends_with(const char *name, size_t name_len, const char *suffix) {
    const size_t suffix_len = strlen(suffix);
    return name_len > suffix_len && memcmp(name + name_len - suffix_len, suffix, suffix_len) == 0;
}

int mesh_ui_journal_forget_all(const struct mesh_ui_journal *journal) {
    if (!mesh_ui_journal_enabled(journal)) {
        return 0;
    }
    DIR *dir = opendir(journal->dir);
    if (dir == NULL) {
        return (errno == ENOENT) ? 0 : -errno;
    }
    char temp_suffix[MESH_UI_JOURNAL_SUFFIX_MAX + sizeof JOURNAL_TEMP_SUFFIX];
    snprintf(temp_suffix, sizeof temp_suffix, "%s" JOURNAL_TEMP_SUFFIX, journal->suffix);

    int dropped = 0;
    const struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        /* Only what this journal writes, matched on the suffix: the directory is the caller's to
           share, but a temporary an interrupted rewrite left behind is ours too and goes with the
           file it was going to replace. */
        const char *name = entry->d_name;
        const size_t name_len = strlen(name);
        const bool is_file = journal_ends_with(name, name_len, journal->suffix);
        const bool is_temp = journal_ends_with(name, name_len, temp_suffix);
        if (!is_file && !is_temp) {
            continue;
        }
        char path[MESH_UI_JOURNAL_DIR_MAX + 256U];
        const int written = snprintf(path, sizeof path, "%s/%s", journal->dir, name);
        if (written <= 0 || (size_t)written >= sizeof path) {
            continue;
        }
        if (remove(path) == 0 && is_file) {
            ++dropped;
        }
    }
    closedir(dir);
    return dropped;
}

/* ---- the ring ------------------------------------------------------------------------------- */

void mesh_ui_journal_ring_init(struct mesh_ui_journal_ring *ring, void *entries, size_t size,
                               uint32_t capacity) {
    if (ring == NULL) {
        return;
    }
    memset(ring, 0, sizeof *ring);
    ring->entries = entries;
    ring->size = size;
    ring->capacity = (entries == NULL || size == 0U) ? 0U : capacity;
}

void *mesh_ui_journal_ring_push(struct mesh_ui_journal_ring *ring) {
    if (ring == NULL || ring->capacity == 0U) {
        return NULL;
    }
    if (ring->wrapped) {
        ring->dropped++;
    }
    void *slot = ring->entries + (size_t)ring->next * ring->size;
    ring->next = (ring->next + 1U) % ring->capacity;
    if (ring->next == 0U) {
        ring->wrapped = true;
    }
    return slot;
}

uint32_t mesh_ui_journal_ring_held(const struct mesh_ui_journal_ring *ring) {
    if (ring == NULL) {
        return 0U;
    }
    return ring->wrapped ? ring->capacity : ring->next;
}

void *mesh_ui_journal_ring_at(const struct mesh_ui_journal_ring *ring, uint32_t i) {
    if (i >= mesh_ui_journal_ring_held(ring)) {
        return NULL;
    }
    return ring->entries + (size_t)i * ring->size;
}

/* Swaps two records a byte at a time, so the rotation needs no scratch record - which for a large
   record is the stack a caller already holding `capacity` of them does not have to spare. */
static void journal_ring_swap(unsigned char *a, unsigned char *b, size_t size) {
    for (size_t i = 0U; i < size; ++i) {
        const unsigned char tmp = a[i];
        a[i] = b[i];
        b[i] = tmp;
    }
}

static void journal_ring_reverse(struct mesh_ui_journal_ring *ring, uint32_t lo, uint32_t hi) {
    for (; lo + 1U < hi; ++lo, --hi) {
        journal_ring_swap(ring->entries + (size_t)lo * ring->size,
                          ring->entries + (size_t)(hi - 1U) * ring->size, ring->size);
    }
}

uint32_t mesh_ui_journal_ring_finish(struct mesh_ui_journal_ring *ring) {
    if (ring == NULL) {
        return 0U;
    }
    if (!ring->wrapped) {
        return ring->next;
    }
    /*
     * Full, with `next` at the oldest entry, so the buffer is rotated left by that much to read
     * oldest-first - as three reversals rather than through a scratch copy, which would be another
     * `capacity` records of stack. Once rotated the ring reads as one that filled exactly, so a
     * second finish is harmless.
     */
    journal_ring_reverse(ring, 0U, ring->next);
    journal_ring_reverse(ring, ring->next, ring->capacity);
    journal_ring_reverse(ring, 0U, ring->capacity);
    ring->next = 0U;
    return ring->capacity;
}
