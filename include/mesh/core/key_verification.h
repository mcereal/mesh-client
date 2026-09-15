#pragma once

/*
 * Proving that the public key we hold for a node is actually theirs.
 *
 * The transcript has drawn a padlock on a PKI-encrypted direct message since direct messages
 * existed, and the padlock has only ever meant "the radio had *a* key for that node and used
 * it". Where the key came from is the question it cannot answer: a key arrives in a NodeInfo
 * from whoever transmitted it, and a node claiming to be somebody else arrives the same way as
 * the real one. Verification is what turns that padlock into a statement about a person -
 * which is why it is a *ceremony* rather than a request, and why every step of it happens
 * somewhere the mesh cannot reach.
 *
 * The exchange, as the firmware runs it and as the phone apps drive it:
 *
 *   1. A's user presses verify. A's client sends INITIATE_VERIFICATION; A's radio picks a
 *      nonce and puts a KeyVerification packet on the mesh.
 *   2. B's radio answers it and tells B's user a **six digit security number**
 *      (`key_verification_number_inform`). B reads it out loud - down a phone, across a table,
 *      anywhere but the mesh.
 *   3. A's radio asks A's user for that number (`key_verification_number_request`). A types
 *      what they heard and the client sends PROVIDE_SECURITY_NUMBER.
 *   4. Both radios finish the handshake and show their user the same short run of
 *      **verification characters** (`key_verification_final`). Both users compare, and each
 *      client answers DO_VERIFY or DO_NOT_VERIFY for its own end.
 *   5. A yes sets the firmware's IS_KEY_MANUALLY_VERIFIED bit, which comes back to us as
 *      `NodeInfo.is_key_manually_verified` and is what the padlock then reads.
 *
 * This file is that exchange as *state*, and nothing else: it does not encode, send, or know
 * what a radio is. The session drives it - it folds the ClientNotifications in, it queues the
 * admin steps - and the UI reads it to know which of the two questions to put in front of the
 * user. Keeping it apart is what lets the whole ceremony be tested without a radio, which
 * matters more here than elsewhere: the failure mode of a verification UI is not a crash, it is
 * a user confidently told that a key is proven when it is not.
 *
 * **One at a time.** The firmware runs a single exchange, so this holds a single exchange, and
 * a notification about a different node replaces what is here rather than queueing behind it -
 * the radio has already moved on, and a sheet showing the previous exchange would be asking
 * about something nothing is waiting for.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The remote node's name as the notification carries it (KeyVerificationFinal.remote_longname,
   40 bytes upstream) plus its NUL. Kept rather than looked up in the roster: a verification is
   about the node the *radio* is talking to, and the name it used is the one the other user is
   looking at while the two of them compare. */
#define MESH_KEY_VERIFICATION_NAME_MAX 41U

/* The characters both ends compare (KeyVerificationFinal.verification_characters, 10 bytes)
   plus its NUL. */
#define MESH_KEY_VERIFICATION_CHARS_MAX 11U

/* How many digits a security number has. The firmware draws it from 1..999999 and writes it
   as two groups of three with leading zeros ("048 172"), so the user reads out six digits
   whatever the value, and the entry field takes exactly that many. */
#define MESH_KEY_VERIFICATION_DIGITS 6U

/*
 * How long an exchange stands before it is given up on.
 *
 * There is nothing on the wire to end one with. A radio whose far end never answered says
 * nothing further, so an exchange left alone would sit in front of the user for ever, and the
 * sheet asking for a number nobody is generating is worse than no sheet: it is a question with
 * no right answer. Five minutes is long enough to find the other person and short enough that a
 * forgotten sheet is gone by the time the device is picked up again.
 */
#define MESH_KEY_VERIFICATION_TIMEOUT_SECONDS 300U

/*
 * Where an exchange has got to, and therefore what - if anything - is being asked of the user.
 *
 * Two of these are questions and the rest are not, which is the distinction every reader of
 * this enum actually cares about; mesh_key_verification_asks() is that question asked once
 * rather than by each caller listing the two.
 */
enum mesh_key_verification_stage {
    MESH_KEY_VERIFICATION_IDLE = 0, /* nothing in flight */
    /* We asked, and the two radios are talking. Nothing for the user to do but wait for their
       own radio to come back with a question. */
    MESH_KEY_VERIFICATION_WAITING,
    /* Our radio generated the security number: read these six digits out to the other person.
       The end that did *not* initiate. */
    MESH_KEY_VERIFICATION_SHOW_NUMBER,
    /* Our radio wants the six digits the other person is reading out. The end that did. */
    MESH_KEY_VERIFICATION_ENTER_NUMBER,
    /* Both radios have finished; both users compare the characters and answer. */
    MESH_KEY_VERIFICATION_COMPARE,
};

/*
 * One exchange.
 *
 * `nonce` is the radio's, not ours: it is 0 between pressing verify and the radio's first
 * question, which is the one moment when an exchange exists here and does not exist on the
 * wire. Everything after that quotes it back, and a step sent without it is refused before it
 * reaches the radio (see mesh_radio_settings_queue_key_verification).
 *
 * `seq` counts stage changes for the life of the session, so a reader can tell "the sheet
 * should now be showing something else" from "the same sheet, repainted" without diffing the
 * struct. It is monotonic and 0 means nothing has happened yet.
 */
struct mesh_key_verification {
    uint8_t stage; /* enum mesh_key_verification_stage */
    /* Which end we are. It decides which question we get asked, and it is worth keeping after
       the fact: a verification we started is one the user is expecting, and one we did not is
       an interruption that arrived while they were doing something else. */
    bool we_initiated;
    uint32_t remote_node;
    uint64_t nonce;
    uint32_t security_number; /* SHOW_NUMBER: ours, to be read out. 0 otherwise. */
    char remote_name[MESH_KEY_VERIFICATION_NAME_MAX];
    char characters[MESH_KEY_VERIFICATION_CHARS_MAX]; /* COMPARE only */
    uint32_t started;                                 /* our clock, epoch seconds */
    uint32_t changed;                                 /* our clock when the stage last moved */
    uint32_t seq;
};

void mesh_key_verification_reset(struct mesh_key_verification *state);

/* True while an exchange is live - anything but IDLE. */
bool mesh_key_verification_active(const struct mesh_key_verification *state);

/* True when the stage is one of the two that puts a question in front of the user: the number
   to type, or the characters to compare. A WAITING exchange is live and asks nothing. */
bool mesh_key_verification_asks(const struct mesh_key_verification *state);

/*
 * The user pressed verify on a node. Takes the slot whatever was in it, for the reason in the
 * header: the radio runs one exchange and this is the one it is about to run. `now` is our
 * clock in epoch seconds; 0 is accepted (a client with no clock cannot expire an exchange, and
 * a ceremony that cannot time out beats one that cannot start).
 *
 * Returns false for a bad argument only - there is no "already busy", because the press is the
 * user telling us which exchange they mean.
 */
bool mesh_key_verification_begin(struct mesh_key_verification *state, uint32_t node_id,
                                 const char *name, uint32_t now);

/*
 * The three ClientNotifications the firmware raises during an exchange, folded in. Each returns
 * true when the stage moved, which is what the caller turns into "open the sheet" or "the sheet
 * now says something else".
 *
 * `name` is the radio's `remote_longname` and may be empty; `characters` likewise. Both are
 * radio text and are sanitised on the way in, exactly as a node name is - a verification sheet
 * is the last screen that should be rendering whatever bytes arrived.
 */
bool mesh_key_verification_on_number_request(struct mesh_key_verification *state, uint64_t nonce,
                                             const char *name, uint32_t now);
bool mesh_key_verification_on_number_inform(struct mesh_key_verification *state, uint64_t nonce,
                                            const char *name, uint32_t security_number,
                                            uint32_t now);
bool mesh_key_verification_on_final(struct mesh_key_verification *state, uint64_t nonce,
                                    const char *name, const char *characters, uint32_t now);

/*
 * Restamps the deadline without moving the stage: the exchange has not changed what it is
 * asking, but something happened, so the five minutes start again.
 *
 * The one caller is the security number being submitted. That queues a step and leaves the
 * stage where it is - the radio is still the one with the next move - so without this the
 * deadline would go on running from the moment the radio *asked* for the number. A user who
 * answered at four minutes fifty-nine would have their own answer expired out from under them
 * a second later, and a DO_NOT_VERIFY queued behind it.
 *
 * Returns false for an idle exchange or a clock of 0, on the same terms as the tick below: a
 * client that cannot measure five minutes has no deadline to restamp.
 */
bool mesh_key_verification_touch(struct mesh_key_verification *state, uint32_t now);

/*
 * The user answered, or backed out. Both end the exchange: there is nothing further to show
 * once the radio has been told, and what comes home afterwards is the node's own NodeInfo with
 * the verified bit in it.
 *
 * `out` (may be NULL) receives the exchange as it stood, because the caller has to name the
 * node in a toast and send the admin step for it after this has cleared the slot.
 */
bool mesh_key_verification_settle(struct mesh_key_verification *state,
                                  struct mesh_key_verification *out);

/*
 * Expires an exchange nothing has moved for MESH_KEY_VERIFICATION_TIMEOUT_SECONDS. Returns
 * true when it took one, with `out` (may be NULL) receiving it so the caller can say which node
 * it was about. A `now` or a `changed` of 0 expires nothing: a client with no clock cannot tell
 * five minutes from five seconds, and guessing would close a sheet under a user's thumb.
 */
bool mesh_key_verification_tick(struct mesh_key_verification *state, uint32_t now,
                                struct mesh_key_verification *out);

#ifdef __cplusplus
}
#endif
