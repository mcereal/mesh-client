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
 * Stats come in four kinds (mesh/core/lifetime.def), and the kind is what decides how much the
 * "once" matters:
 *
 *   - A COUNT is not idempotent: told twice, it is wrong. It relies on the session entirely.
 *   - A MAX is: the largest of a value seen twice is the same value. So is a MIN, the smallest.
 *   - A SET is too: a node heard twice is one node. Its number is a count of the seen file.
 *
 * **Two files, in one directory, over inkstand's journal**, so a card that cannot hold the
 * directory leaves the stats disabled and quiet, as the other logs are:
 *
 *   `totals`  the COUNTs and MAXes, and whether the set has ever turned a node away (a restart
 *             cannot tell that from a set that is merely at its size). A few hundred bytes,
 * rewritten whole through a temporary when it has changed, on the app's two-second batching window
 * - so a SIGKILL or a pulled battery costs at most that window. `seen`    the SET: one line each
 * time a node gains a fact (heard at all, heard over the air, heard with no relay between, is one
 * of our radios). Appended as it happens, since it happens rarely, and never rewritten; an append
 * that fails is retried by the next flush. Read back into a sorted array at launch; a torn last
 * line is skipped, and a line read twice changes nothing.
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
 * **A delivery is where a direct message is now.** The session announces each of our messages
 * changing delivery state, once, with the state it left (MESH_SESSION_EVENT_DELIVERY). A direct
 * message is counted when it leaves pending - it asked to be confirmed, which a reaction and a
 * broadcast never do - and from then on moves between the two counts as its answer changes (a
 * late reply to a command given up on, a relay's acknowledgement then the recipient's refusal),
 * so it is one message in whichever count its bubble shows. Only a message this run counted is
 * ever moved: one counted before a restart or a reset stays where it was left rather than
 * taking a count that is some other message's. "Delivered" is what the bubble says, which for a
 * direct message may be a relay's acknowledgement rather than the recipient's.
 *
 * **A record is a MAX or a MIN**, and both are kept the same way: whether one has been set, who
 * set it and when. A MIN is the one kind that is signed - the weakest signal decoded is a
 * signal-to-noise ratio, which is below zero when a packet is decoded from under the noise, and
 * the quietest is a strength in dBm, which nearly always is - so its value is read with
 * mesh_lifetime_signed() and written to the card with its sign.
 *
 * **A signal belongs to whoever transmitted it**, which for a relayed packet is the last relay
 * rather than the node it is from. So the two signal records take only packets that came
 * straight to our radio, where the node a record names is the one that was that faint; the
 * farthest-heard record takes any hop count, since a distance is between the two ends.
 *
 * **An absence is measured off the roster**, which is the one place a node's last hearing is
 * kept: the session hands over the node's `last_heard` from before the packet with the record
 * after it, and the gap between the two is how long the node went unheard. The roster outlives
 * a run, so a node heard again after the client was off for a week was absent for that week as
 * far as this client knows - and a radio whose NodeDB heard it meanwhile says so at the next
 * listing, which moves `last_heard` on before any packet arrives. Both ends have to be on a
 * receiver's clock, so only Meshtastic reports one: MeshCore stamps a contact the radio never
 * modified with the advert's own time, on the sender's clock, and a gap between two clocks is
 * their difference rather than the node's absence.
 *
 * **A trace counts once it is answered.** The session announces a traceroute when the answer
 * lands (MESH_SESSION_EVENT_TRACE), from either protocol, and a trace that times out is never
 * announced - an unanswered trace says nothing about the mesh, only that the request or its
 * answer was lost somewhere. The longest trace is in relays, as the most-hops record is, and is
 * the longer way of the two: an answer may come back by another route than the request took.
 *
 * **Time on a link is the one thing the session does not feed**, because the session does not
 * know it has a link: it is told about frames, not about a transport coming and going. So the
 * app says, each turn and each frame, whether a link is up (mesh_lifetime_note_link()), and the
 * stats keep the edges themselves. That is a feed from what *is*, which the rule above warns
 * against, and it is safe here for the reason a COUNT is not: an edge is counted when the state
 * changes, not each time it is read, and the time is a difference between two readings of the
 * loop's monotonic clock that is banked once and never read back. A restart starts with the link
 * down, so the first turn that sees one up is a new connection, which it is. Time is banked in
 * whole seconds, a minute at a time while the link stays up and in full when it drops or the
 * app closes it - so the card is rewritten once a minute for time rather than on every turn,
 * and a SIGKILL or a pulled battery costs at most that minute. Time asleep is not time
 * connected: the monotonic clock stops with the processor, and a link does not survive a
 * suspend anyway.
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
    MESH_LIFETIME_MIN,
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
    /* COUNT, MAX and MIN - a MIN's as an int64_t, which mesh_lifetime_signed() reads back; a
       SET's slot is unused. */
    uint64_t values[MESH_LIFETIME_STAT_COUNT];
    /* A record (a MAX or a MIN) something has set, even to 0; unused for the other kinds. */
    bool measured[MESH_LIFETIME_STAT_COUNT];
    /* A record's holder: the node that set it, and the credible second it did (0 when there was
       no clock). A holder of 0 is one this card cannot name. Unused for the other kinds. */
    uint32_t holders[MESH_LIFETIME_STAT_COUNT];
    uint32_t held_at[MESH_LIFETIME_STAT_COUNT];
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
    /*
     * The direct messages this run has counted as delivered or failed, and which: the only ones
     * a later change may move between the two. As long as the session's message log, which is
     * as far back as a message can still change - one it has evicted can no longer be found to
     * be marked. A slot is reused only once its message has left the log, so every message the
     * log still holds keeps its place. Not on the card, and emptied by a reset.
     */
    uint32_t settled_ids[MESH_MESSAGE_LOG_CAPACITY];
    uint8_t settled_in[MESH_MESSAGE_LOG_CAPACITY]; /* enum mesh_lifetime_stat, or 0xFF: neither */
    /* The link as the last mesh_lifetime_note_link() saw it: whether one was up, when it came
       up and up to when its time is already in CONNECTED_S (monotonic ms). Not on the card. A
       reset clears them with everything else, so a link up across a reset is the first
       connection of the stats that start then, timed from the next sample - "starts again from
       today" includes the link that is up today. */
    bool link_up;
    uint64_t link_since_ms;
    uint64_t link_banked_ms;
    /* The radio the link reaches, as the session announced it (mesh_lifetime_note_radio()) on
       this link and never before it: 0 until then, which leaves a record set by that stretch
       with nobody to name rather than the last radio's name. An announcement while no link is
       up waits in `link_radio_next` for the next one, which is how the reset's re-announcement
       and a handshake frame handled just before the link was sampled still reach the link
       they belong to: the next link to come up takes it, and no later one. */
    uint32_t link_radio;
    uint32_t link_radio_next;
    /* This link's stretch is what LONGEST_CONNECTION_S holds - it beat the record rather than
       tying one - so a radio announced after the record was set, with nobody named, is named on
       it: a tie keeps its holder, so the stretch's last bank would otherwise leave it blank. */
    bool link_holds_record;
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
   the radio it talked to - and it is the only way a link learns its radio: the session says it
   once per handshake, so it is never the last radio's number still in the handshake status. */
void mesh_lifetime_note_radio(struct mesh_lifetime *lifetime, uint32_t node_num);

/*
 * Whether a link is up now, told as often as the app likes. A link coming up is a CONNECTION; while
 * it stays up its time is banked into CONNECTED_S a minute at a time, and the stretch as a whole
 * raises LONGEST_CONNECTION_S, a record held by the radio the session announced on it. A link going
 * down banks what is left. `now_ms` is inkwell's monotonic clock; a reading that goes backwards is
 * ignored rather than wrapped. The app tells it the link is down as it shuts down, so a clean exit
 * banks the last seconds.
 */
#define MESH_LIFETIME_LINK_BANK_MS 60000U
void mesh_lifetime_note_link(struct mesh_lifetime *lifetime, bool up, uint64_t now_ms);

/* A COUNT, a MAX or a SET. A MIN is signed and reads as 0 here: see mesh_lifetime_signed(). */
uint64_t mesh_lifetime_value(const struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat);
/* A MIN, with its sign; 0 for every other kind, and for a MIN nothing has set yet. */
int64_t mesh_lifetime_signed(const struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat);
enum mesh_lifetime_kind mesh_lifetime_kind_of(enum mesh_lifetime_stat stat);
/* The stat's key on the card, for logs and tests. NULL out of range. */
const char *mesh_lifetime_key(enum mesh_lifetime_stat stat);

/*
 * Whether a record (a MAX or a MIN) has ever been set, which its value cannot say: 0 is both
 * "not yet" and a real record (a node only ever heard straight to us is a most-hops of 0). Always
 * true for a COUNT and a SET, whose 0 means none. Kept across a restart; a reset clears it.
 */
bool mesh_lifetime_measured(const struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat);

/*
 * Who set a record and when: the node the record came from, and the wall-clock second it was set,
 * or 0 when the device had no credible clock then. False when there is nobody to name - a
 * COUNT or a SET, a record not measured yet, or one read off a card written before holders
 * were kept.
 *
 * A tie keeps the holder it has. A record is the first to reach it, not the latest, and a
 * record that changed hands every time somebody else matched it would name whoever was heard
 * last rather than whoever did it.
 */
bool mesh_lifetime_holder(const struct mesh_lifetime *lifetime, enum mesh_lifetime_stat stat,
                          uint32_t *out_node, uint32_t *out_at);

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
