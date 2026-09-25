#ifndef MESH_UI_STORE_RECENT_H
#define MESH_UI_STORE_RECENT_H

/*
 * A bounded most-recently-used list: the handful of things a program was last used with, newest
 * first, the oldest pushed off the end when a new one arrives, and written to a file as one line.
 *
 * The first application on this stack kept two - the peers it had talked to, and the addresses it
 * had reached them at - and wrote the same list twice: the same move-to-front, the same "already
 * at the head, nothing to write" that every publish hits, the same shift that drops the oldest,
 * the same forget, the same rank, and the same comma-separated line read back in file order. This
 * is that half. What an entry *is*, when two entries are the same one, and how one is spelled in
 * the file stay with whoever keeps the list, as the callbacks below.
 *
 * Three decisions are the reason it is shaped the way it is:
 *
 *   - **The caller owns the storage.** A list is a view over an array the caller declared, with
 *     the count beside it, so a record that has always carried `entries[8]` and a `count` byte
 *     keeps them and nothing here allocates. The size of one entry is bounded by
 *     MESH_UI_RECENT_ENTRY_MAX so a note can copy the entry it was handed before anything moves:
 *     the natural call passes a pointer into the very list being reordered.
 *
 *   - **The order is the value.** A rank is how recently, not whether: 0 is the newest and -1 is
 *     "not one of these". A list read from a file is taken in file order rather than replayed
 *     through note(), which would reverse it.
 *
 *   - **A note says whether anything changed.** Noting the entry that is already at the head is
 *     the common case - it happens on every refresh - and reports nothing to write, so the caller
 *     rewrites its file only when the list actually moved.
 *
 * Not threads: one loop owns a list.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest entry a list will hold, in bytes. A note copies the entry onto the stack before it
   shifts anything, and this is what bounds that copy. */
#define MESH_UI_RECENT_ENTRY_MAX 256U

/* Whether `entry` (one of the list's) is the same thing as `wanted`. NULL compares every byte.
   A caller whose identity is looser than its bytes - an address that is the same in either case,
   a kind that is part of the name - says so here. */
typedef bool (*mesh_ui_recent_same_fn)(const void *entry, const void *wanted, void *context);

struct mesh_ui_recent {
    /* `capacity` entries of `size` bytes each, newest first. */
    void *entries;
    size_t size;
    size_t capacity;
    /* How many of them are in use. Clamped to `capacity` by every call. */
    size_t count;
    mesh_ui_recent_same_fn same;
    void *context;
};

/* A list over `entries`, holding `count` of them already. Returns -EINVAL for a NULL array, an
   empty capacity or one past INT_MAX (a rank is an int), or an entry size of 0 or past
   MESH_UI_RECENT_ENTRY_MAX. */
int mesh_ui_recent_init(struct mesh_ui_recent *list, void *entries, size_t size, size_t capacity,
                        size_t count, mesh_ui_recent_same_fn same, void *context);

/* How recently `wanted` was noted: 0 is the newest, -1 is not in the list. */
int mesh_ui_recent_rank(const struct mesh_ui_recent *list, const void *wanted);

/* Puts `entry` at the head. One already in the list moves there, taking the caller's copy of it;
   a new one pushes the oldest off the end of a full list. Returns true when the list changed, and
   false when `entry` was already the head - which is left as it was. */
bool mesh_ui_recent_note(struct mesh_ui_recent *list, const void *entry);

/* Drops `wanted`, closing the gap and zeroing the slot the list no longer uses. Returns true when
   it was there. */
bool mesh_ui_recent_forget(struct mesh_ui_recent *list, const void *wanted);

/* Writes one entry to `out`. Returns 0 or a negative errno. It must not write the separator. */
typedef int (*mesh_ui_recent_write_fn)(FILE *out, const void *entry, void *context);

/* Writes every entry, newest first, with `separator` between them and nothing after the last -
   the caller writes its own key before and its own line end after. Returns 0 or the first
   negative errno, -EIO when the stream refused the separator. */
int mesh_ui_recent_write(const struct mesh_ui_recent *list, FILE *out, char separator,
                         mesh_ui_recent_write_fn write, void *context);

/* Reads the `len` bytes at `text` - one entry as write() spelled it, not terminated - into
   `entry`, which arrives zeroed. Returns false to skip it. */
typedef bool (*mesh_ui_recent_parse_fn)(const char *text, size_t len, void *entry, void *context);

/* Replaces the list with the entries in `text`, split on `separator`, in file order. An entry the
   parser refuses is skipped, a second copy of one already read is dropped, and reading stops at
   capacity. Every slot past the new count is zeroed. Returns the new count. */
size_t mesh_ui_recent_parse(struct mesh_ui_recent *list, const char *text, char separator,
                            mesh_ui_recent_parse_fn parse, void *context);

#ifdef __cplusplus
}
#endif

#endif
