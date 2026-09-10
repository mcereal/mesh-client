#pragma once

/*
 * What the *radio's* firmware situation is: which board this is, what it is running, what the
 * newest release is, and whether this client could install it from where it is standing.
 *
 * The client's own self-update is next door in updater.h and this borrows its shape - one
 * child at a time through the event loop, one state per thing a row can name - because they
 * are the same problem twice. What they are not is the same feature: this one changes a
 * different computer, over a bus, and getting it wrong there is somebody's radio rather than a
 * relaunch. docs/radio-firmware-roadmap.md is the whole plan.
 *
 * **This is phase 1 of that plan and it installs nothing.** It reads two documents and reports
 * what they say, which is most of the value for anybody who owns a computer and all of the
 * value for anybody wondering why their radio is behaving the way it is. Everything the states
 * below do not mention - the download, the handover, the loader - arrives in later phases and
 * arrives as more states, not as a second module.
 *
 * Two answers, deliberately kept apart:
 *
 *   - `state` is how the *check* went, and it ends at up-to-date, a newer release, or a
 *     failure.
 *   - `blocker` is whether we could act on it, and it is a different question with a different
 *     answer. A board upstream has stopped building for still gets a version row; a board on
 *     the wrong bus still gets told which bus to use. A refusal is a row, not silence - the
 *     client says which of them is true.
 */

#include "mesh/core/fetch.h"
#include "mesh/core/firmware_catalog.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

/* One line for the UI: what happened, or why it did not. */
#define MESH_FIRMWARE_MESSAGE_MAX 96U

/* Where a check has got to. A row names one of these, so each is a state a reader can act on. */
enum mesh_firmware_state {
    MESH_FIRMWARE_IDLE = 0, /* never checked this run */
    /* Reading the hardware document: turning this radio's hw_model into a board. */
    MESH_FIRMWARE_IDENTIFYING,
    /* Reading the release index: what the newest firmware is. */
    MESH_FIRMWARE_CHECKING,
    MESH_FIRMWARE_UP_TO_DATE,
    MESH_FIRMWARE_AVAILABLE, /* a newer release exists; `release` names it */
    MESH_FIRMWARE_FAILED,    /* `message` says why */
    MESH_FIRMWARE_STATE_COUNT,
};

/*
 * Why this client could not install what it just found, if it could not.
 *
 * Every one of these is a row rather than a silence, and they are decided in the order they
 * are declared: most fundamental first, which puts the most actionable last. From "no radio"
 * through "I do not know what this is" to "there is an image and a way, just not this cable".
 * The order is not cosmetic - telling somebody to go and find a USB cable for a board upstream
 * publishes no image for would be a lie with a chore attached.
 */
enum mesh_firmware_blocker {
    /* There is a path, and phase 3 or 4 is what will walk it. Until then every check ends
       here with nothing to press, which is what phase 1 being phase 1 means. */
    MESH_FIRMWARE_BLOCKER_NONE = 0,
    /* No radio, or one that has not said what it is yet. */
    MESH_FIRMWARE_BLOCKER_NO_RADIO,
    /* The hardware document has no board claiming this radio's hw_model. Either the radio is
       newer than the document, or it is a build that never registered one. */
    MESH_FIRMWARE_BLOCKER_UNKNOWN_BOARD,
    /* Several boards claim it and nobody has said which. Never resolved by guessing: flashing
       the wrong variant of the right board is the failure this feature must not have. */
    MESH_FIRMWARE_BLOCKER_AMBIGUOUS,
    /* Upstream has stopped building for this board, so there is no image to install whatever
       bus you are on. It still reports what it is running. */
    MESH_FIRMWARE_BLOCKER_UNSUPPORTED_BOARD,
    /* Not from here at all: ESP32-C3 and C6, whose loader partition holds a build current
       firmware refuses to boot into, and portduino, which is a Linux process. */
    MESH_FIRMWARE_BLOCKER_NO_PATH,
    /* This board has a path, but not over the bus we are connected on. Last because it is the
       most useful refusal there is: it names a thing the user can go and do. */
    MESH_FIRMWARE_BLOCKER_WRONG_BUS,
    MESH_FIRMWARE_BLOCKER_COUNT,
};

struct mesh_firmware {
    struct mesh_fetch fetch;
    enum mesh_firmware_state state;
    enum mesh_firmware_channel channel;
    /*
     * The bus the client is talking to the radio over, borrowing the path enum: a serial link
     * is over the same USB port a UF2 write would use, and a BLE link is where an OTA would
     * happen, so "can this be done from here" is one comparison rather than a table. NONE when
     * nothing is connected.
     */
    enum mesh_firmware_path bus;

    /* What the radio said about itself when the check started, kept so the answer cannot drift
       under the rows while a radio reconnects mid-check. */
    uint32_t hw_model;
    char running[MESH_FIRMWARE_VERSION_MAX];

    /*
     * The caller's clock, stamped at every check and every tick.
     *
     * The two documents are one press and the second is started from the first's completion,
     * which arrives on the read path with no time to hand - and a deadline has to be measured
     * against the same clock as the tick that enforces it. Keeping the caller's rather than
     * reading one here is what lets a test drive the whole check on a clock it chose.
     */
    uint64_t now_ms;

    struct mesh_firmware_boards boards;
    struct mesh_firmware_release release;
    enum mesh_firmware_blocker blocker;
    char message[MESH_FIRMWARE_MESSAGE_MAX];

    /* Bumped whenever anything above changes, so app.c can publish without diffing. */
    uint32_t revision;
};

/* `loop` may be NULL, in which case the module reports itself unavailable. Returns 0, or
   -errno. */
int mesh_firmware_init(struct mesh_firmware *firmware, struct mesh_event_loop *loop);
void mesh_firmware_shutdown(struct mesh_firmware *firmware);

/*
 * The CA bundle to verify with. Resolved once per process by whoever found it - on the Brick
 * that is the pak's own, because the device has no system store - and handed here rather than
 * looked up again: where the bundle is is a fact about how this binary was installed, and two
 * modules answering it separately is two answers that can disagree.
 */
void mesh_firmware_use_ca_bundle(struct mesh_firmware *firmware, const char *path);

/* True when a fetcher was found, i.e. when a check could do anything at all. */
bool mesh_firmware_available(const struct mesh_firmware *firmware);
/* True while a document is in flight. */
bool mesh_firmware_busy(const struct mesh_firmware *firmware);

/*
 * Tells the module which bus the radio is on, as the app sees it. Cheap and idempotent; a
 * change forgets nothing, because the check's *answer* does not depend on the bus - only the
 * blocker does, and that is recomputed from here.
 */
void mesh_firmware_set_bus(struct mesh_firmware *firmware, enum mesh_firmware_path bus);

/*
 * Starts a check for the radio described by `hw_model` and `running`.
 *
 * Both come from `DeviceMetadata` and both may be absent: a `hw_model` of 0 is a radio that
 * has not said, and an empty `running` sorts below every release, so a check on either still
 * reports the newest firmware and says the board could not be identified. No-op while one is
 * in flight. Returns 0, or -errno.
 */
int mesh_firmware_check(struct mesh_firmware *firmware, uint32_t hw_model, const char *running,
                        uint64_t now_ms);

/* Enforces the per-document timeout and reaps a finished child. Call every loop turn. */
void mesh_firmware_tick(struct mesh_firmware *firmware, uint64_t now_ms);

/*
 * True when the answer being held was computed for the radio described by `hw_model` and
 * `running` - that is, when it is still worth showing.
 *
 * Both, because both went into it: the model decided which board this is and the version
 * decided whether the newest release was news. Either one moving makes the whole answer stale,
 * and the model alone is not enough - two identical boards on different firmware share a model,
 * and a caller testing only that would leave the version row reading the new radio while the
 * row under it reported a verdict computed from the old one. It is also what catches a radio
 * that updated its own firmware, which is not a swap at all and stales the answer just the
 * same.
 *
 * Always true at IDLE: there is no answer to be stale.
 */
bool mesh_firmware_answers_for(const struct mesh_firmware *firmware, uint32_t hw_model,
                               const char *running);

/*
 * Drops whatever the last check concluded, back to idle.
 *
 * For a radio swap: the board, the version and the blocker were all answers about a node that
 * is no longer on the other end, and a row still naming the old one is worse than a row saying
 * nothing yet. Refuses while a check is running, which is the same rule the updater's channel
 * switch follows - the document in flight belongs to the question that started it.
 */
void mesh_firmware_forget(struct mesh_firmware *firmware);

/*
 * The single board this radio is, or NULL when there is not exactly one - because it could not
 * be identified, or because several claim it and nobody has chosen. The ambiguous case
 * deliberately answers NULL rather than the first candidate.
 */
const struct mesh_firmware_board *mesh_firmware_board(const struct mesh_firmware *firmware);

const char *mesh_firmware_state_name(enum mesh_firmware_state state);
/* The one line a row shows for a blocker, or an empty string for NONE. */
const char *mesh_firmware_blocker_reason(enum mesh_firmware_blocker blocker);

#ifdef __cplusplus
}
#endif
