#pragma once

/*
 * What this client has seen over its whole life: every radio, every run, never reset by a swap.
 *
 * Everything else the client keeps is a window. The transport ring holds 64 messages, a
 * conversation's log 512, a node's trend half a day, and the roster is whichever radio is
 * attached. Each forgets on purpose. These are the numbers that only ever go up - messages sent,
 * nodes heard, the farthest a packet has come straight to us - and they are about the client,
 * not about a radio, so a second radio adds to them rather than starting them again.
 *
 * **Fed from what happens, never from what is.** The one rule everything below turns on. A
 * count read off state drifts both ways: the ring evicts before a publish sees a message, and a
 * roster reloaded from the card at launch would be counted again every launch. So this is the
 * session's observer (struct mesh_session_event) and nothing else feeds it - the session says a
 * thing once, at the moment it records it, and never for a record it was handed back.
 *
 * Stats come in three kinds (mesh/core/lifetime.def), and the kind is what decides how much the
 * "once" matters:
 *
 *   - A COUNT is not idempotent: told twice, it is wrong. It relies on the session entirely.
 *   - A MAX is: the largest of a value seen twice is the same value.
 *   - A SET is too: a node heard twice is one node. Its number is a count of the seen file.
 *
 * **Two files, in one directory, over inkstand's journal**, so a card that cannot hold the
 * directory leaves the stats disabled and quiet, as the other logs are:
 *
 *   `totals`  the COUNTs and MAXes, and whether the set has ever turned a node away (a restart
 *             cannot tell that from a set that is merely at its size). A few hundred bytes,
 * rewritten whole through a temporary when it has changed, on the app's two-second batching window
 * - so a SIGKILL or a pulled battery costs at most that window. `seen`    the SET: one line each
 * time a node gains a fact (heard at all, heard over the air, is one of our radios). Appended as it
 * happens, since it happens rarely, and never rewritten; an append that fails is retried by the
 * next flush. Read back into a sorted array at launch; a torn last line is skipped, and a line read
 * twice changes nothing.
 *
 * **A node is its 32-bit number.** Meshtastic takes it from the radio's hardware and MeshCore
 * from the front of a public key, so the two protocols share one set; two nodes landing on one
 * number is a chance in four billion per pair, and would cost one node from a count.
 *
 * **Our own radios are not nodes we heard.** A radio that has ever been attached is marked as
 * one, and the node counts leave it out - at count time rather than at insert time, so a second
 * radio heard over the air before it was ever plugged in stops counting the day it is.
 *
 * **Only a clock that is credibly one is written down**, and nothing here depends on it. `since`
 * is the first credible wall-clock second the stats saw, and stays 0 on a device that has never
 * had one; every count moves exactly the same either way.
 *
 * What is deliberately not counted: a node the radio lists with no heard time (a contact typed
 * in from a link), a Store & Forward replay as a hearing of its author, hops or distance over
 * MQTT (neither says anything about this radio's reach), and our own radio's echo of a send.
 * A distance is between two fixes as the nodes reported them, and a node that rounds its
 * position off (precision_bits) moves it by up to that much.
 */

#include "inkstand/persist/journal.h"
#include "mesh/core/session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_lifetime_stat {
#define MESH_LIFETIME_STAT(ID, key, kind) MESH_LIFETIME_##ID,
#include "mesh/core/lifetime.def"
#undef MESH_LIFETIME_STAT
    MESH_LIFETIME_STAT_COUNT
};

enum mesh_lifetime_kind {
    MESH_LIFETIME_COUNT = 0,
    MESH_LIFETIME_MAX,
    MESH_LIFETIME_SET,
};

/*
 * How many distinct nodes the set remembers. Past it a new node is not remembered, the counts
 * become a floor, and mesh_lifetime_complete() says so. Eight thousand is several years of a
 * busy city mesh, and 40 KB.
 */
#define MESH_LIFETIME_NODES_MAX 8192U

/* Keys a newer build wrote that this one does not know, carried through a rewrite rather than
   dropped - a card moved back to an older build keeps what the newer one counted. */
#define MESH_LIFETIME_FOREIGN_MAX 16U
#define MESH_LIFETIME_FOREIGN_KEY 32U
#define MESH_LIFETIME_FOREIGN_VALUE 24U

struct mesh_lifetime {
    struct inkstand_journal journal;
    uint64_t values[MESH_LIFETIME_STAT_COUNT]; /* COUNT and MAX; a SET's slot is unused */
    /* A MAX something has set, even to 0; unused for the other kinds. */
    bool measured[MESH_LIFETIME_STAT_COUNT];
    uint32_t since; /* first credible wall-clock second, or 0 */
    /* The set: node numbers ascending, and what is known about each (lifetime.c's SEEN_*). */
    uint32_t ids[MESH_LIFETIME_NODES_MAX];
    uint8_t facts[MESH_LIFETIME_NODES_MAX];
    uint32_t id_count;
    /* Nodes with a fact the seen file does not hold yet, because its append failed. */
    uint32_t pending;
    bool full;
    char foreign_keys[MESH_LIFETIME_FOREIGN_MAX][MESH_LIFETIME_FOREIGN_KEY];
    char foreign_values[MESH_LIFETIME_FOREIGN_MAX][MESH_LIFETIME_FOREIGN_VALUE];
    uint32_t foreign_count;
    /* `totals` differs from the card. */
    bool dirty;
    /* Moves whenever any value does, so a screen can ask whether to redraw. */
    uint32_t revision;
};

/*
 * Opens the stats in `dir` (made if missing) and reads both files back. A missing file is a
 * fresh start, not an error. 0, or the negative errno that left the stats disabled - in which
 * case they still count for this run and write nothing.
 */
int mesh_lifetime_init(struct mesh_lifetime *lifetime, const char *dir);

/* The session observer (mesh_session_observer_fn); `ctx` is the struct mesh_lifetime. */
void mesh_lifetime_observe(void *ctx, const struct mesh_session *session,
                           const struct mesh_session_event *event);

/* A radio this client has been attached to. Idempotent; 0 is ignored. The observer calls it
   for the session's RADIO event, which is how a run that never publishes a frame still counts
   the radio it talked to. */
void mesh_lifetime_note_radio(struct mesh_lifetime *lifetime, uint32_t node_num);

uint64_t mesh_lifetime_value(const struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat);
enum mesh_lifetime_kind mesh_lifetime_kind_of(enum mesh_lifetime_stat stat);
/* The stat's key on the card, for logs and tests. NULL out of range. */
const char *mesh_lifetime_key(enum mesh_lifetime_stat stat);

/*
 * Whether a MAX has ever been set, which its value cannot say: 0 is both "not yet" and a real
 * record (a node only ever heard straight to us is a most-hops of 0). Always true for a COUNT and
 * a SET, whose 0 means none. Kept across a restart; a reset clears it.
 */
bool mesh_lifetime_measured(const struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat);

/* False once the set has had to turn a node away, which makes every SET a floor. Kept across a
   restart; only mesh_lifetime_reset() makes the set complete again. */
bool mesh_lifetime_complete(const struct mesh_lifetime *lifetime);

/* Whether `totals` has something the card does not. */
bool mesh_lifetime_dirty(const struct mesh_lifetime *lifetime);

/* Writes `totals` if it is dirty, and retries any node fact whose append failed. 0 or a
   negative errno; whatever failed stays dirty and is tried again at the next flush. */
int mesh_lifetime_flush(struct mesh_lifetime *lifetime);

/* Starts again from nothing: both files go, and every value is 0. 0 or a negative errno. */
int mesh_lifetime_reset(struct mesh_lifetime *lifetime);

#ifdef __cplusplus
}
#endif
