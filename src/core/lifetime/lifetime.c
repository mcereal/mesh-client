#define _POSIX_C_SOURCE 200809L

#include "mesh/core/lifetime.h"

#include "inkwell/base/log.h"
#include "inkwell/base/record_file.h"
#include "inkwell/base/time.h"
#include "mesh/core/message.h"
#include "mesh/geo/vector.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIFETIME_SUFFIX ".stats"
#define LIFETIME_TOTALS "totals"
#define LIFETIME_SEEN "seen"
#define LIFETIME_KEY_SINCE "since"
#define LIFETIME_LINE_MAX 128U

/* What the set knows about a node. Each is one line in the seen file, keyed by its name. */
enum {
    SEEN_HEARD = 1U << 0, /* heard at all: over the air, over MQTT, or listed by a radio */
    SEEN_RF = 1U << 1,    /* heard over the air, by a radio of ours */
    SEEN_RADIO = 1U << 2, /* one of our own radios */
};

static const struct {
    uint8_t fact;
    const char *key;
} k_seen_keys[] = {
    {SEEN_HEARD, "heard"},
    {SEEN_RF, "rf"},
    {SEEN_RADIO, "radio"},
};

static const char *const k_stat_keys[MESH_LIFETIME_STAT_COUNT] = {
#define MESH_LIFETIME_STAT(ID, key, kind) [MESH_LIFETIME_##ID] = key,
#include "mesh/core/lifetime.def"
#undef MESH_LIFETIME_STAT
};

static const enum mesh_lifetime_kind k_stat_kinds[MESH_LIFETIME_STAT_COUNT] = {
#define MESH_LIFETIME_STAT(ID, key, kind) [MESH_LIFETIME_##ID] = MESH_LIFETIME_##kind,
#include "mesh/core/lifetime.def"
#undef MESH_LIFETIME_STAT
};

const char *mesh_lifetime_key(enum mesh_lifetime_stat stat) {
    return (unsigned)stat < MESH_LIFETIME_STAT_COUNT ? k_stat_keys[stat] : NULL;
}

enum mesh_lifetime_kind mesh_lifetime_kind_of(enum mesh_lifetime_stat stat) {
    return (unsigned)stat < MESH_LIFETIME_STAT_COUNT ? k_stat_kinds[stat] : MESH_LIFETIME_COUNT;
}

static void lifetime_changed(struct mesh_lifetime *lifetime) { lifetime->revision++; }

/* The first credible second, written down once. The clock may become credible mid-run - a
   device whose time is set over Wi-Fi after launch - so this is asked on every event. */
static void lifetime_note_since(struct mesh_lifetime *lifetime) {
    if (lifetime->since != 0U) {
        return;
    }
    const uint32_t now = inkwell_time_wall_credible_s();
    if (now != 0U) {
        lifetime->since = now;
        lifetime->dirty = true;
    }
}

/* ------------------------------------------------------------------------------- the set */

/* Where `id` is, or where it would go: the first slot not below it. */
static uint32_t lifetime_find(const struct mesh_lifetime *lifetime, uint32_t id) {
    uint32_t low = 0U;
    uint32_t high = lifetime->id_count;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2U;
        if (lifetime->ids[mid] < id) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    return low;
}

/* Merges `facts` into `id`'s entry and answers the ones it did not already have. 0 when there
   is nothing new, including when the set is full and `id` is not in it. */
static uint8_t lifetime_learn(struct mesh_lifetime *lifetime, uint32_t id, uint8_t facts) {
    const uint32_t at = lifetime_find(lifetime, id);
    if (at < lifetime->id_count && lifetime->ids[at] == id) {
        const uint8_t fresh = (uint8_t)(facts & ~lifetime->facts[at]);
        lifetime->facts[at] |= fresh;
        return fresh;
    }
    if (lifetime->id_count >= MESH_LIFETIME_NODES_MAX) {
        if (!lifetime->full) {
            inkwell_log_warn("lifetime", "The node set is full; node counts are a floor from here");
        }
        lifetime->full = true;
        return 0U;
    }
    const size_t tail = (size_t)(lifetime->id_count - at);
    memmove(&lifetime->ids[at + 1U], &lifetime->ids[at], tail * sizeof lifetime->ids[0]);
    memmove(&lifetime->facts[at + 1U], &lifetime->facts[at], tail * sizeof lifetime->facts[0]);
    lifetime->ids[at] = id;
    lifetime->facts[at] = facts;
    lifetime->id_count++;
    return facts;
}

struct lifetime_seen_line {
    uint32_t id;
    uint8_t facts;
};

static void lifetime_write_seen(FILE *file, bool resumed, void *context) {
    (void)resumed;
    const struct lifetime_seen_line *line = context;
    for (size_t i = 0; i < sizeof k_seen_keys / sizeof k_seen_keys[0]; ++i) {
        if ((line->facts & k_seen_keys[i].fact) != 0U) {
            fprintf(file, "%s=%08" PRIx32 "\n", k_seen_keys[i].key, line->id);
        }
    }
}

/* Learns and, when that taught the set anything, writes it down at once: a new node is rare, and
   one that reached the card only at the next flush would be lost to a SIGKILL. */
static void lifetime_record(struct mesh_lifetime *lifetime, uint32_t id, uint8_t facts) {
    if (id == 0U || id == MESH_MESSAGE_BROADCAST_ADDR) {
        return;
    }
    const uint8_t fresh = lifetime_learn(lifetime, id, facts);
    if (fresh == 0U) {
        return;
    }
    lifetime_changed(lifetime);
    struct lifetime_seen_line line = {.id = id, .facts = fresh};
    const int result = inkstand_journal_append(&lifetime->journal, LIFETIME_SEEN,
                                               lifetime_write_seen, &line, NULL);
    if (result < 0) {
        inkwell_log_debug("lifetime", "Could not write a node to the seen file: %d", result);
    }
}

static void lifetime_read_seen(void *context, const char *key, char *value) {
    struct mesh_lifetime *lifetime = context;
    for (size_t i = 0; i < sizeof k_seen_keys / sizeof k_seen_keys[0]; ++i) {
        if (strcmp(key, k_seen_keys[i].key) == 0) {
            char *end = NULL;
            const unsigned long id = strtoul(value, &end, 16);
            if (end != value && *end == '\0' && id != 0UL && id <= UINT32_MAX) {
                (void)lifetime_learn(lifetime, (uint32_t)id, k_seen_keys[i].fact);
            }
            return;
        }
    }
}

static uint64_t lifetime_count_nodes(const struct mesh_lifetime *lifetime, uint8_t fact) {
    uint64_t count = 0U;
    for (uint32_t i = 0; i < lifetime->id_count; ++i) {
        const uint8_t facts = lifetime->facts[i];
        if ((facts & fact) != 0U && (fact == SEEN_RADIO || (facts & SEEN_RADIO) == 0U)) {
            count++;
        }
    }
    return count;
}

/* --------------------------------------------------------------------------- the totals */

static void lifetime_bump(struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat) {
    if (lifetime->values[stat] < UINT64_MAX) {
        lifetime->values[stat]++;
    }
    lifetime->dirty = true;
    lifetime_changed(lifetime);
}

static void lifetime_raise(struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat,
                           uint64_t value) {
    if (value > lifetime->values[stat]) {
        lifetime->values[stat] = value;
        lifetime->dirty = true;
        lifetime_changed(lifetime);
    }
}

static void lifetime_read_totals(void *context, const char *key, char *value) {
    struct mesh_lifetime *lifetime = context;
    if (strcmp(key, LIFETIME_KEY_SINCE) == 0) {
        char *end = NULL;
        const unsigned long long since = strtoull(value, &end, 10);
        if (end != value && *end == '\0' && since <= UINT32_MAX) {
            lifetime->since = (uint32_t)since;
        }
        return;
    }
    for (size_t i = 0; i < MESH_LIFETIME_STAT_COUNT; ++i) {
        if (strcmp(key, k_stat_keys[i]) != 0) {
            continue;
        }
        char *end = NULL;
        const unsigned long long parsed = strtoull(value, &end, 10);
        if (k_stat_kinds[i] != MESH_LIFETIME_SET && end != value && *end == '\0') {
            lifetime->values[i] = parsed;
        }
        return;
    }
    if (lifetime->foreign_count < MESH_LIFETIME_FOREIGN_MAX &&
        strlen(key) < MESH_LIFETIME_FOREIGN_KEY && strlen(value) < MESH_LIFETIME_FOREIGN_VALUE) {
        const uint32_t at = lifetime->foreign_count++;
        snprintf(lifetime->foreign_keys[at], MESH_LIFETIME_FOREIGN_KEY, "%s", key);
        snprintf(lifetime->foreign_values[at], MESH_LIFETIME_FOREIGN_VALUE, "%s", value);
    }
}

static void lifetime_write_totals(FILE *file, void *context) {
    const struct mesh_lifetime *lifetime = context;
    fprintf(file, "%s=%" PRIu32 "\n", LIFETIME_KEY_SINCE, lifetime->since);
    for (size_t i = 0; i < MESH_LIFETIME_STAT_COUNT; ++i) {
        if (k_stat_kinds[i] != MESH_LIFETIME_SET) {
            fprintf(file, "%s=%" PRIu64 "\n", k_stat_keys[i], lifetime->values[i]);
        }
    }
    for (uint32_t i = 0; i < lifetime->foreign_count; ++i) {
        fprintf(file, "%s=", lifetime->foreign_keys[i]);
        inkwell_record_write_escaped(file, lifetime->foreign_values[i]);
        fputc('\n', file);
    }
}

/* ------------------------------------------------------------------------------- the API */

int mesh_lifetime_init(struct mesh_lifetime *lifetime, const char *dir) {
    if (lifetime == NULL) {
        return -EINVAL;
    }
    memset(lifetime, 0, sizeof *lifetime);
    const int opened = inkstand_journal_init(&lifetime->journal, dir, LIFETIME_SUFFIX, 0U);
    char line[LIFETIME_LINE_MAX];
    int result = inkstand_journal_read(&lifetime->journal, LIFETIME_TOTALS, line, sizeof line,
                                       lifetime_read_totals, lifetime);
    if (result < 0 && result != -ENOENT) {
        inkwell_log_warn("lifetime", "Could not read the totals: %d", result);
    }
    result = inkstand_journal_read(&lifetime->journal, LIFETIME_SEEN, line, sizeof line,
                                   lifetime_read_seen, lifetime);
    if (result < 0 && result != -ENOENT) {
        inkwell_log_warn("lifetime", "Could not read the seen file: %d", result);
    }
    lifetime_note_since(lifetime);
    return opened;
}

void mesh_lifetime_note_radio(struct mesh_lifetime *lifetime, uint32_t node_num) {
    if (lifetime == NULL) {
        return;
    }
    lifetime_record(lifetime, node_num, SEEN_RADIO);
}

/* How far `node` is from our own radio, when both have said where they are. */
static bool lifetime_distance(const struct mesh_session *session,
                              const struct mesh_node_summary *node, uint64_t *out_m) {
    const struct mesh_handshake_status *status = &session->handshake;
    if (!status->has_my_info || !node->position.valid) {
        return false;
    }
    for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        const struct mesh_node_summary *self = &status->nodes[i];
        if (self->node_id != status->my_info.my_node_num) {
            continue;
        }
        struct mesh_geo_vector vector;
        if (!self->position.valid ||
            !mesh_geo_vector_between(self->position.latitude_i, self->position.longitude_i,
                                     node->position.latitude_i, node->position.longitude_i,
                                     &vector)) {
            return false;
        }
        *out_m = (uint64_t)(vector.distance_m + 0.5);
        return true;
    }
    return false;
}

static void lifetime_observe_message(struct mesh_lifetime *lifetime,
                                     const struct mesh_message *message) {
    const bool outbound = message->direction == MESH_MESSAGE_OUTBOUND;
    if (message->is_reaction) {
        lifetime_bump(lifetime,
                      outbound ? MESH_LIFETIME_REACTIONS_SENT : MESH_LIFETIME_REACTIONS_RECEIVED);
        return;
    }
    lifetime_bump(lifetime,
                  outbound ? MESH_LIFETIME_MESSAGES_SENT : MESH_LIFETIME_MESSAGES_RECEIVED);
    if (message->to != MESH_MESSAGE_BROADCAST_ADDR) {
        lifetime_bump(lifetime,
                      outbound ? MESH_LIFETIME_DIRECT_SENT : MESH_LIFETIME_DIRECT_RECEIVED);
    }
}

void mesh_lifetime_observe(void *ctx, const struct mesh_session *session,
                           const struct mesh_session_event *event) {
    struct mesh_lifetime *lifetime = ctx;
    if (lifetime == NULL || session == NULL || event == NULL) {
        return;
    }
    lifetime_note_since(lifetime);

    if (event->kind == MESH_SESSION_EVENT_MESSAGE) {
        if (event->message != NULL) {
            lifetime_observe_message(lifetime, event->message);
        }
        return;
    }
    const struct mesh_node_summary *node = event->node;
    if (node == NULL) {
        return;
    }
    lifetime_record(lifetime, node->node_id,
                    (uint8_t)(SEEN_HEARD | (event->via_mqtt ? 0U : SEEN_RF)));

    /* The path records are about this radio's reach, so they take only what we watched arrive,
       over the air; a listing or a bridged packet says nothing about either. */
    if (event->kind != MESH_SESSION_EVENT_NODE_HEARD || event->via_mqtt || !event->has_hops) {
        return;
    }
    lifetime_raise(lifetime, MESH_LIFETIME_MOST_HOPS, event->hops);
    uint64_t distance_m = 0U;
    if (event->hops == 0U && lifetime_distance(session, node, &distance_m)) {
        lifetime_raise(lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M, distance_m);
    }
}

uint64_t mesh_lifetime_value(const struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat) {
    if (lifetime == NULL || (unsigned)stat >= MESH_LIFETIME_STAT_COUNT) {
        return 0U;
    }
    switch (stat) {
    case MESH_LIFETIME_NODES_HEARD:
        return lifetime_count_nodes(lifetime, SEEN_HEARD);
    case MESH_LIFETIME_NODES_HEARD_RF:
        return lifetime_count_nodes(lifetime, SEEN_RF);
    case MESH_LIFETIME_RADIOS:
        return lifetime_count_nodes(lifetime, SEEN_RADIO);
    default:
        return lifetime->values[stat];
    }
}

bool mesh_lifetime_complete(const struct mesh_lifetime *lifetime) {
    return lifetime != NULL && !lifetime->full;
}

bool mesh_lifetime_dirty(const struct mesh_lifetime *lifetime) {
    return lifetime != NULL && lifetime->dirty;
}

int mesh_lifetime_flush(struct mesh_lifetime *lifetime) {
    if (lifetime == NULL) {
        return -EINVAL;
    }
    if (!lifetime->dirty) {
        return 0;
    }
    const int result = inkstand_journal_replace(&lifetime->journal, LIFETIME_TOTALS,
                                                lifetime_write_totals, lifetime);
    if (result == 0) {
        lifetime->dirty = false;
    }
    return result;
}

int mesh_lifetime_reset(struct mesh_lifetime *lifetime) {
    if (lifetime == NULL) {
        return -EINVAL;
    }
    const int removed = inkstand_journal_forget_all(&lifetime->journal);
    const struct inkstand_journal journal = lifetime->journal;
    const uint32_t revision = lifetime->revision;
    memset(lifetime, 0, sizeof *lifetime);
    lifetime->journal = journal;
    lifetime->revision = revision + 1U;
    lifetime_note_since(lifetime);
    return removed < 0 ? removed : 0;
}
