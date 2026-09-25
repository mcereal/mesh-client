#include "mesh/ui/store_recent.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static unsigned char *slot(const struct mesh_ui_recent *list, size_t index) {
    return (unsigned char *)list->entries + index * list->size;
}

static bool same(const struct mesh_ui_recent *list, const void *entry, const void *wanted) {
    return list->same != NULL ? list->same(entry, wanted, list->context)
                              : memcmp(entry, wanted, list->size) == 0;
}

/* A count past the array is a record written by something else, or a byte that went bad on the
   card; either way no index at or past the capacity is ever read. */
static void clamp(struct mesh_ui_recent *list) {
    if (list->count > list->capacity) {
        list->count = list->capacity;
    }
}

/* The rank among the first `count` entries only, which is what a parse asks while the entry it
   is testing sits in the slot just past them. */
static int rank_within(const struct mesh_ui_recent *list, size_t count, const void *wanted) {
    for (size_t i = 0; i < count; ++i) {
        if (same(list, slot(list, i), wanted)) {
            return (int)i;
        }
    }
    return -1;
}

int mesh_ui_recent_init(struct mesh_ui_recent *list, void *entries, size_t size, size_t capacity,
                        size_t count, mesh_ui_recent_same_fn same_fn, void *context) {
    if (list == NULL || entries == NULL || size == 0U || size > MESH_UI_RECENT_ENTRY_MAX ||
        capacity == 0U || capacity > (size_t)INT_MAX) {
        return -EINVAL;
    }
    list->entries = entries;
    list->size = size;
    list->capacity = capacity;
    list->count = count;
    list->same = same_fn;
    list->context = context;
    clamp(list);
    return 0;
}

int mesh_ui_recent_rank(const struct mesh_ui_recent *list, const void *wanted) {
    if (list == NULL || list->entries == NULL || wanted == NULL) {
        return -1;
    }
    const size_t count = list->count < list->capacity ? list->count : list->capacity;
    return rank_within(list, count, wanted);
}

bool mesh_ui_recent_note(struct mesh_ui_recent *list, const void *entry) {
    if (list == NULL || list->entries == NULL || entry == NULL) {
        return false;
    }
    clamp(list);
    if (list->count > 0U && same(list, slot(list, 0), entry)) {
        return false; /* already the most recent; the common case, every refresh */
    }

    /* By value before anything moves: the entry may well point into the list this is about to
       shift, and the copy into the head below would then read whatever slid into its place. */
    unsigned char wanted[MESH_UI_RECENT_ENTRY_MAX];
    memcpy(wanted, entry, list->size);

    /* Slide everything ahead of the existing entry down by one and put this one in front. One not
       seen before pushes the oldest off the end. */
    int existing = rank_within(list, list->count, wanted);
    size_t shift_from;
    if (existing >= 0) {
        shift_from = (size_t)existing;
    } else {
        if (list->count < list->capacity) {
            list->count++;
        }
        shift_from = list->count - 1U;
    }
    if (shift_from > 0U) {
        memmove(slot(list, 1), slot(list, 0), shift_from * list->size);
    }
    memcpy(slot(list, 0), wanted, list->size);
    return true;
}

bool mesh_ui_recent_forget(struct mesh_ui_recent *list, const void *wanted) {
    if (list == NULL || list->entries == NULL || wanted == NULL) {
        return false;
    }
    clamp(list);
    const int rank = rank_within(list, list->count, wanted);
    if (rank < 0) {
        return false;
    }
    const size_t after = list->count - (size_t)rank - 1U;
    if (after > 0U) {
        memmove(slot(list, (size_t)rank), slot(list, (size_t)rank + 1U), after * list->size);
    }
    list->count--;
    memset(slot(list, list->count), 0, list->size);
    return true;
}

int mesh_ui_recent_write(const struct mesh_ui_recent *list, FILE *out, char separator,
                         mesh_ui_recent_write_fn write, void *context) {
    if (list == NULL || list->entries == NULL || out == NULL || write == NULL) {
        return -EINVAL;
    }
    const size_t count = list->count < list->capacity ? list->count : list->capacity;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0U && fputc(separator, out) == EOF) {
            return -EIO;
        }
        const int written = write(out, slot(list, i), context);
        if (written < 0) {
            return written;
        }
    }
    return 0;
}

size_t mesh_ui_recent_parse(struct mesh_ui_recent *list, const char *text, char separator,
                            mesh_ui_recent_parse_fn parse, void *context) {
    if (list == NULL || list->entries == NULL) {
        return 0U;
    }
    list->count = 0U;
    memset(list->entries, 0, list->capacity * list->size);
    if (text == NULL || parse == NULL) {
        return 0U;
    }

    const char *cursor = text;
    while (*cursor != '\0' && list->count < list->capacity) {
        const char *end = strchr(cursor, separator);
        const size_t len = end != NULL ? (size_t)(end - cursor) : strlen(cursor);

        /* Parsed straight into the first free slot, and kept by counting it: a refused entry or
           a second copy of one already read is zeroed again and the slot is reused. */
        unsigned char *free_slot = slot(list, list->count);
        if (parse(cursor, len, free_slot, context) &&
            rank_within(list, list->count, free_slot) < 0) {
            list->count++;
        } else {
            memset(free_slot, 0, list->size);
        }
        cursor = end != NULL ? end + 1 : cursor + len;
    }
    return list->count;
}
