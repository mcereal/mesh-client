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
/* Written once the set has turned a node away, and never cleared but by a reset. The seen file
   cannot say it: a set that is full holds exactly MESH_LIFETIME_NODES_MAX nodes either way, and
   only the refusal - which appends nothing - tells the two apart. */
#define LIFETIME_KEY_FULL "nodes_full"
/*
 * Appended to a MAX's key once something has set it, even to 0: "<key>.measured=1".
 *
 * A record's value alone cannot say it. 0 is both "nothing measured yet" and a real record - a
 * node heard only ever straight to us is a most-hops of 0 - and a screen that guessed from the
 * value, or from some other count, draws "none yet" over a measurement. A line of its own
 * rather than a new spelling of the value, so an older build carries it through as a key it
 * does not know, and a card written before it existed still reads: see lifetime_read_legacy().
 */
#define LIFETIME_MEASURED_SUFFIX ".measured"
/*
 * And who set the record, and when, as three lines:
 *
 *   <key>.holder=<node hex>       the node
 *   <key>.held_value=<value>      the record it was the holder *of*
 *   <key>.held_at=<second>        the credible second it was set, or 0
 *
 * Three short lines rather than one, because a build that does not know them keeps each only as
 * a foreign value under MESH_LIFETIME_FOREIGN_VALUE characters - a card moved back to such a
 * build and forward again must still name the holder, and one line joining all three is
 * already past that on an ordinary distance record.
 *
 * The holder is believed only while `held_value` is still the record. Such a build carries the
 * three lines through untouched while raising the record itself, so without it the new record
 * would be credited to the node that held the old one. A holder that no longer matches is
 * dropped rather than guessed at.
 */
#define LIFETIME_HOLDER_SUFFIX ".holder"
#define LIFETIME_HELD_VALUE_SUFFIX ".held_value"
#define LIFETIME_HELD_AT_SUFFIX ".held_at"
#define LIFETIME_LINE_MAX 128U

/* What the set knows about a node. Each is one line in the seen file, keyed by its name. */
enum {
    SEEN_HEARD = 1U << 0, /* heard at all: over the air, over MQTT, or listed by a radio */
    SEEN_RF = 1U << 1,    /* heard over the air, by a radio of ours */
    SEEN_RADIO = 1U << 2, /* one of our own radios */
};

/* The same facts, learned but not yet on the card: an append that failed. Kept beside the fact
   rather than instead of it, so the count is right now and the next flush writes it down. */
#define SEEN_PENDING_SHIFT 4U
#define SEEN_PENDING(facts) ((uint8_t)((facts) >> SEEN_PENDING_SHIFT))

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

/* A MAX or a MIN: a stat that is a record, with a holder and a "measured" of its own. */
static bool lifetime_is_record(size_t stat) {
    return k_stat_kinds[stat] == MESH_LIFETIME_MAX || k_stat_kinds[stat] == MESH_LIFETIME_MIN;
}

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
            lifetime->full = true;
            lifetime->dirty = true;
            lifetime_changed(lifetime);
        }
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

static int lifetime_append_seen(struct mesh_lifetime *lifetime, uint32_t id, uint8_t facts) {
    struct lifetime_seen_line line = {.id = id, .facts = facts};
    return inkstand_journal_append(&lifetime->journal, LIFETIME_SEEN, lifetime_write_seen, &line,
                                   NULL);
}

/*
 * Learns and, when that taught the set anything, writes it down at once: a new node is rare, and
 * one that reached the card only at the next flush would be lost to a SIGKILL.
 *
 * An append that fails leaves the fact pending rather than forgotten. The set already holds it,
 * so the next hearing of the node would find nothing fresh and never write it - and the SET
 * values are not in the totals, so a fact that never reached the seen file is lost at the next
 * launch. The flush retries it, and marking the stats dirty is what gets a flush scheduled.
 */
static void lifetime_record(struct mesh_lifetime *lifetime, uint32_t id, uint8_t facts) {
    if (id == 0U || id == MESH_MESSAGE_BROADCAST_ADDR) {
        return;
    }
    const uint8_t fresh = lifetime_learn(lifetime, id, facts);
    if (fresh == 0U) {
        return;
    }
    lifetime_changed(lifetime);
    const int result = lifetime_append_seen(lifetime, id, fresh);
    if (result < 0) {
        inkwell_log_debug("lifetime", "Could not write a node to the seen file: %d", result);
        const uint32_t at = lifetime_find(lifetime, id);
        if (SEEN_PENDING(lifetime->facts[at]) == 0U) {
            lifetime->pending++;
        }
        lifetime->facts[at] |= (uint8_t)(fresh << SEEN_PENDING_SHIFT);
        lifetime->dirty = true;
    }
}

/* Writes every pending fact down. 0, or the first failure; what failed stays pending. */
static int lifetime_retry_seen(struct mesh_lifetime *lifetime) {
    int first_error = 0;
    for (uint32_t i = 0; i < lifetime->id_count && lifetime->pending > 0U; ++i) {
        const uint8_t pending = SEEN_PENDING(lifetime->facts[i]);
        if (pending == 0U) {
            continue;
        }
        const int result = lifetime_append_seen(lifetime, lifetime->ids[i], pending);
        if (result < 0) {
            if (first_error == 0) {
                first_error = result;
            }
            continue;
        }
        lifetime->facts[i] &= (uint8_t)((1U << SEEN_PENDING_SHIFT) - 1U);
        lifetime->pending--;
    }
    return first_error;
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

/* Whether `value` beats a record of `current`: larger for a MAX, smaller for a MIN, which holds
   an int64_t. A tie never does - see mesh_lifetime_holder(). */
static bool lifetime_beats(enum mesh_lifetime_stat stat, uint64_t value, uint64_t current) {
    if (k_stat_kinds[stat] == MESH_LIFETIME_MIN) {
        return (int64_t)value < (int64_t)current;
    }
    return value > current;
}

/* Moves a record to `value` when it beats it, or is the first measurement, and credits `holder`
   with it. A MIN's `value` is an int64_t's bits. */
static void lifetime_raise(struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat,
                           uint64_t value, uint32_t holder) {
    if (!lifetime->measured[stat] || lifetime_beats(stat, value, lifetime->values[stat])) {
        lifetime->values[stat] = value;
        lifetime->measured[stat] = true;
        lifetime->holders[stat] = holder;
        lifetime->held_at[stat] = inkwell_time_wall_credible_s();
        lifetime->dirty = true;
        lifetime_changed(lifetime);
    }
}

/* "<key><suffix>" for a record, or false: whether `key` is one of a record's own lines. */
static bool lifetime_record_key(const char *key, const char *suffix_text, size_t *out_stat) {
    const size_t len = strlen(key);
    const size_t suffix = strlen(suffix_text);
    if (len <= suffix || strcmp(key + len - suffix, suffix_text) != 0) {
        return false;
    }
    for (size_t i = 0; i < MESH_LIFETIME_STAT_COUNT; ++i) {
        if (lifetime_is_record(i) && strlen(k_stat_keys[i]) == len - suffix &&
            strncmp(key, k_stat_keys[i], len - suffix) == 0) {
            *out_stat = i;
            return true;
        }
    }
    return false;
}

/* A card written before the marker existed has none, so a record it holds above 0 was measured
   - nothing else raises one - and a 0 is taken as the "none yet" it most likely was. */
static void lifetime_read_legacy(struct mesh_lifetime *lifetime) {
    for (size_t i = 0; i < MESH_LIFETIME_STAT_COUNT; ++i) {
        if (k_stat_kinds[i] == MESH_LIFETIME_MAX && lifetime->values[i] > 0U) {
            lifetime->measured[i] = true;
        }
    }
}

/* The holder lines read off the card, held until every line is in: the value they name may come
   after them, and is what decides whether they are believed. */
enum {
    HOLDER_READ_NODE = 1U << 0,
    HOLDER_READ_VALUE = 1U << 1,
    HOLDER_READ_AT = 1U << 2,
    HOLDER_READ_ALL = HOLDER_READ_NODE | HOLDER_READ_VALUE | HOLDER_READ_AT,
};

struct lifetime_totals_read {
    struct mesh_lifetime *lifetime;
    uint64_t holder_values[MESH_LIFETIME_STAT_COUNT];
    uint8_t holder_read[MESH_LIFETIME_STAT_COUNT];
};

/* A whole unsigned decimal or hex number, and no more than `max`. False on anything else. */
static bool lifetime_parse_number(const char *text, int base, uint64_t max, uint64_t *out) {
    char *end = NULL;
    const unsigned long long parsed = strtoull(text, &end, base);
    if (end == text || *end != '\0' || text[0] == '-' || parsed > max) {
        return false;
    }
    *out = parsed;
    return true;
}

/* A stat's value as the card spells it: decimal, and signed for a MIN, whose bits it answers. */
static bool lifetime_parse_value(size_t stat, const char *text, uint64_t *out) {
    if (k_stat_kinds[stat] != MESH_LIFETIME_MIN) {
        return lifetime_parse_number(text, 10, UINT64_MAX, out);
    }
    char *end = NULL;
    errno = 0;
    const long long parsed = strtoll(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = (uint64_t)(int64_t)parsed;
    return true;
}

static void lifetime_write_value(FILE *file, size_t stat, uint64_t value) {
    if (k_stat_kinds[stat] == MESH_LIFETIME_MIN) {
        fprintf(file, "%" PRId64, (int64_t)value);
    } else {
        fprintf(file, "%" PRIu64, value);
    }
}

/* One of a record's three holder lines, or false when `key` is none of them. */
static bool lifetime_read_holder(struct lifetime_totals_read *read, const char *key,
                                 const char *value) {
    struct mesh_lifetime *lifetime = read->lifetime;
    size_t record = 0U;
    uint64_t parsed = 0U;
    if (lifetime_record_key(key, LIFETIME_HOLDER_SUFFIX, &record)) {
        if (lifetime_parse_number(value, 16, UINT32_MAX, &parsed) && parsed != 0U) {
            lifetime->holders[record] = (uint32_t)parsed;
            read->holder_read[record] |= HOLDER_READ_NODE;
        }
        return true;
    }
    if (lifetime_record_key(key, LIFETIME_HELD_VALUE_SUFFIX, &record)) {
        if (lifetime_parse_value(record, value, &parsed)) {
            read->holder_values[record] = parsed;
            read->holder_read[record] |= HOLDER_READ_VALUE;
        }
        return true;
    }
    if (lifetime_record_key(key, LIFETIME_HELD_AT_SUFFIX, &record)) {
        if (lifetime_parse_number(value, 10, UINT32_MAX, &parsed)) {
            lifetime->held_at[record] = (uint32_t)parsed;
            read->holder_read[record] |= HOLDER_READ_AT;
        }
        return true;
    }
    return false;
}

/* Keeps a holder only where it names the record the card now holds. */
static void lifetime_settle_holders(struct lifetime_totals_read *read) {
    struct mesh_lifetime *lifetime = read->lifetime;
    for (size_t i = 0; i < MESH_LIFETIME_STAT_COUNT; ++i) {
        if (read->holder_read[i] != HOLDER_READ_ALL || !lifetime->measured[i] ||
            read->holder_values[i] != lifetime->values[i]) {
            lifetime->holders[i] = 0U;
            lifetime->held_at[i] = 0U;
        }
    }
}

static void lifetime_read_totals(void *context, const char *key, char *value) {
    struct lifetime_totals_read *read = context;
    struct mesh_lifetime *lifetime = read->lifetime;
    if (strcmp(key, LIFETIME_KEY_SINCE) == 0) {
        char *end = NULL;
        const unsigned long long since = strtoull(value, &end, 10);
        if (end != value && *end == '\0' && since <= UINT32_MAX) {
            lifetime->since = (uint32_t)since;
        }
        return;
    }
    if (strcmp(key, LIFETIME_KEY_FULL) == 0) {
        lifetime->full = strcmp(value, "1") == 0;
        return;
    }
    size_t record = 0U;
    if (lifetime_record_key(key, LIFETIME_MEASURED_SUFFIX, &record)) {
        lifetime->measured[record] = strcmp(value, "1") == 0;
        return;
    }
    if (lifetime_read_holder(read, key, value)) {
        return;
    }
    for (size_t i = 0; i < MESH_LIFETIME_STAT_COUNT; ++i) {
        if (strcmp(key, k_stat_keys[i]) != 0) {
            continue;
        }
        uint64_t parsed = 0U;
        if (k_stat_kinds[i] != MESH_LIFETIME_SET && lifetime_parse_value(i, value, &parsed)) {
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
    if (lifetime->full) {
        fprintf(file, "%s=1\n", LIFETIME_KEY_FULL);
    }
    for (size_t i = 0; i < MESH_LIFETIME_STAT_COUNT; ++i) {
        if (k_stat_kinds[i] != MESH_LIFETIME_SET) {
            fprintf(file, "%s=", k_stat_keys[i]);
            lifetime_write_value(file, i, lifetime->values[i]);
            fputc('\n', file);
        }
        if (lifetime_is_record(i) && lifetime->measured[i]) {
            fprintf(file, "%s%s=1\n", k_stat_keys[i], LIFETIME_MEASURED_SUFFIX);
        }
        if (lifetime_is_record(i) && lifetime->measured[i] && lifetime->holders[i] != 0U) {
            fprintf(file, "%s%s=%08" PRIx32 "\n", k_stat_keys[i], LIFETIME_HOLDER_SUFFIX,
                    lifetime->holders[i]);
            fprintf(file, "%s%s=", k_stat_keys[i], LIFETIME_HELD_VALUE_SUFFIX);
            lifetime_write_value(file, i, lifetime->values[i]);
            fputc('\n', file);
            fprintf(file, "%s%s=%" PRIu32 "\n", k_stat_keys[i], LIFETIME_HELD_AT_SUFFIX,
                    lifetime->held_at[i]);
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
    struct lifetime_totals_read read = {.lifetime = lifetime};
    int result = inkstand_journal_read(&lifetime->journal, LIFETIME_TOTALS, line, sizeof line,
                                       lifetime_read_totals, &read);
    if (result < 0 && result != -ENOENT) {
        inkwell_log_warn("lifetime", "Could not read the totals: %d", result);
    }
    lifetime_read_legacy(lifetime);
    lifetime_settle_holders(&read);
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

/*
 * One message appended. Of the ones received, two more counts are kept: those that came over
 * MQTT rather than from our own radio's air, and the direct ones encrypted with our key rather
 * than a channel's, which nobody else on the mesh could read. Received only - a message we send
 * is on the log before the radio has decided either.
 */
static void lifetime_observe_message(struct mesh_lifetime *lifetime,
                                     const struct mesh_message *message, bool via_mqtt) {
    const bool outbound = message->direction == MESH_MESSAGE_OUTBOUND;
    if (message->is_reaction) {
        lifetime_bump(lifetime,
                      outbound ? MESH_LIFETIME_REACTIONS_SENT : MESH_LIFETIME_REACTIONS_RECEIVED);
        return;
    }
    lifetime_bump(lifetime,
                  outbound ? MESH_LIFETIME_MESSAGES_SENT : MESH_LIFETIME_MESSAGES_RECEIVED);
    const bool direct = message->to != MESH_MESSAGE_BROADCAST_ADDR;
    if (direct) {
        lifetime_bump(lifetime,
                      outbound ? MESH_LIFETIME_DIRECT_SENT : MESH_LIFETIME_DIRECT_RECEIVED);
    }
    if (outbound) {
        return;
    }
    if (via_mqtt) {
        lifetime_bump(lifetime, MESH_LIFETIME_RECEIVED_MQTT);
    }
    if (direct && message->pki_encrypted) {
        lifetime_bump(lifetime, MESH_LIFETIME_DIRECT_RECEIVED_PRIVATE);
    }
}

#define LIFETIME_SETTLED_NEITHER 0xFFU

/* The count a delivery state is in: delivered, failed, or neither (waiting, or never asked). */
static uint8_t lifetime_delivery_count(uint8_t ack) {
    if (ack == MESH_MESSAGE_ACK_DELIVERED) {
        return (uint8_t)MESH_LIFETIME_MESSAGES_DELIVERED;
    }
    if (ack == MESH_MESSAGE_ACK_FAILED) {
        return (uint8_t)MESH_LIFETIME_MESSAGES_FAILED;
    }
    return LIFETIME_SETTLED_NEITHER;
}

/* Where this run counted `packet_id`, or NULL for a message it never counted. */
static uint8_t *lifetime_settled(struct mesh_lifetime *lifetime, uint32_t packet_id) {
    for (size_t i = 0; i < MESH_MESSAGE_LOG_CAPACITY; ++i) {
        if (lifetime->settled_ids[i] == packet_id) {
            return &lifetime->settled_in[i];
        }
    }
    return NULL;
}

/*
 * A slot for a message newly counted: an empty one, or one whose message the session's log no
 * longer holds and so can never be marked again. Never a message still in the log - that one can
 * still change its answer, and would find nothing to move. The log holds at most as many messages
 * as there are slots, and the new one is among them, so a slot is always free.
 */
static uint8_t *lifetime_settle_slot(struct mesh_lifetime *lifetime,
                                     const struct mesh_message_log *log, uint32_t **out_id) {
    for (size_t i = 0; i < MESH_MESSAGE_LOG_CAPACITY; ++i) {
        if (lifetime->settled_ids[i] == 0U) {
            *out_id = &lifetime->settled_ids[i];
            return &lifetime->settled_in[i];
        }
    }
    for (size_t i = 0; i < MESH_MESSAGE_LOG_CAPACITY; ++i) {
        const uint32_t id = lifetime->settled_ids[i];
        bool held = false;
        for (size_t n = 0; n < log->count && n < MESH_MESSAGE_LOG_CAPACITY; ++n) {
            const struct mesh_message *entry =
                &log->entries[(log->head + n) % MESH_MESSAGE_LOG_CAPACITY];
            if (entry->packet_id == id && entry->direction == MESH_MESSAGE_OUTBOUND) {
                held = true;
                break;
            }
        }
        if (!held) {
            *out_id = &lifetime->settled_ids[i];
            return &lifetime->settled_in[i];
        }
    }
    return NULL;
}

/*
 * One of our direct messages changed delivery state.
 *
 * Counted when it leaves pending: it asked to be confirmed and now has an answer. A reaction is
 * sent asking for nothing, and a broadcast is confirmed by nobody - this client sends one without
 * want_ack, and MeshCore's pending on a channel message is its place in the radio's queue - so
 * either would reach a count only by failing, and neither is counted.
 *
 * After that the message moves between the counts as its answer changes: MeshCore takes a late
 * reply to a command it gave up on as a delivery, and a relay's acknowledgement can be followed
 * by the recipient's refusal. Each is one message, in whichever count its bubble now shows. Only
 * a message this run counted is moved, because only for those is it known which count holds it:
 * one counted before a restart or a reset would otherwise take a count that is another's.
 */
static void lifetime_observe_delivery(struct mesh_lifetime *lifetime,
                                      const struct mesh_session *session,
                                      const struct mesh_message *message, uint8_t previous) {
    if (message->to == MESH_MESSAGE_BROADCAST_ADDR || message->packet_id == 0U) {
        return;
    }
    const uint8_t joined = lifetime_delivery_count(message->ack);
    uint8_t *held = lifetime_settled(lifetime, message->packet_id);
    if (held == NULL) {
        if (previous != MESH_MESSAGE_ACK_PENDING || joined == LIFETIME_SETTLED_NEITHER) {
            return;
        }
        uint32_t *id = NULL;
        uint8_t *slot = lifetime_settle_slot(lifetime, &session->messages, &id);
        if (slot == NULL) {
            return;
        }
        *id = message->packet_id;
        *slot = joined;
        lifetime_bump(lifetime, (enum mesh_lifetime_stat)joined);
        return;
    }
    if (*held == joined) {
        return;
    }
    if (*held != LIFETIME_SETTLED_NEITHER && lifetime->values[*held] > 0U) {
        lifetime->values[*held]--;
    }
    *held = joined;
    if (joined != LIFETIME_SETTLED_NEITHER) {
        lifetime_bump(lifetime, (enum mesh_lifetime_stat)joined);
    } else {
        lifetime->dirty = true;
        lifetime_changed(lifetime);
    }
}

void mesh_lifetime_observe(void *ctx, const struct mesh_session *session,
                           const struct mesh_session_event *event) {
    struct mesh_lifetime *lifetime = ctx;
    if (lifetime == NULL || session == NULL || event == NULL) {
        return;
    }
    lifetime_note_since(lifetime);

    if (event->kind == MESH_SESSION_EVENT_RADIO) {
        mesh_lifetime_note_radio(lifetime, event->radio);
        return;
    }
    if (event->kind == MESH_SESSION_EVENT_MESSAGE) {
        if (event->message != NULL) {
            lifetime_observe_message(lifetime, event->message, event->via_mqtt);
        }
        return;
    }
    if (event->kind == MESH_SESSION_EVENT_DELIVERY) {
        if (event->message != NULL) {
            lifetime_observe_delivery(lifetime, session, event->message, event->previous_ack);
        }
        return;
    }
    const struct mesh_node_summary *node = event->node;
    if (node == NULL) {
        return;
    }
    lifetime_record(lifetime, node->node_id,
                    (uint8_t)(SEEN_HEARD | (event->via_mqtt ? 0U : SEEN_RF)));

    /* The records are about this radio's reach, so they take only what we watched arrive, over
       the air; a listing or a bridged packet says nothing about any of them. */
    if (event->kind != MESH_SESSION_EVENT_NODE_HEARD || event->via_mqtt) {
        return;
    }
    uint64_t distance_m = 0U;
    const bool has_distance = lifetime_distance(session, node, &distance_m);
    if (has_distance) {
        lifetime_raise(lifetime, MESH_LIFETIME_FARTHEST_HEARD_M, distance_m, node->node_id);
    }
    if (!event->has_hops) {
        return;
    }
    lifetime_raise(lifetime, MESH_LIFETIME_MOST_HOPS, event->hops, node->node_id);
    if (event->hops != 0U) {
        return;
    }
    if (has_distance) {
        lifetime_raise(lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M, distance_m, node->node_id);
    }
    if (event->has_snr) {
        const int64_t quarters = (int64_t)(event->snr * 4.0f + (event->snr < 0.0f ? -0.5f : 0.5f));
        lifetime_raise(lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB, (uint64_t)quarters, node->node_id);
    }
}

uint64_t mesh_lifetime_value(const struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat) {
    if (lifetime == NULL || (unsigned)stat >= MESH_LIFETIME_STAT_COUNT) {
        return 0U;
    }
    if (k_stat_kinds[stat] == MESH_LIFETIME_MIN) {
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

int64_t mesh_lifetime_signed(const struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat) {
    if (lifetime == NULL || (unsigned)stat >= MESH_LIFETIME_STAT_COUNT ||
        k_stat_kinds[stat] != MESH_LIFETIME_MIN) {
        return 0;
    }
    return (int64_t)lifetime->values[stat];
}

bool mesh_lifetime_measured(const struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat) {
    if (lifetime == NULL || (unsigned)stat >= MESH_LIFETIME_STAT_COUNT) {
        return false;
    }
    return !lifetime_is_record(stat) || lifetime->measured[stat];
}

bool mesh_lifetime_holder(const struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat,
                          uint32_t *out_node, uint32_t *out_at) {
    if (lifetime == NULL || (unsigned)stat >= MESH_LIFETIME_STAT_COUNT ||
        !lifetime_is_record(stat) || !lifetime->measured[stat] || lifetime->holders[stat] == 0U) {
        return false;
    }
    if (out_node != NULL) {
        *out_node = lifetime->holders[stat];
    }
    if (out_at != NULL) {
        *out_at = lifetime->held_at[stat];
    }
    return true;
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
    const int seen = lifetime_retry_seen(lifetime);
    const int result = inkstand_journal_replace(&lifetime->journal, LIFETIME_TOTALS,
                                                lifetime_write_totals, lifetime);
    if (result == 0 && seen == 0) {
        lifetime->dirty = false;
    }
    return result < 0 ? result : seen;
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
