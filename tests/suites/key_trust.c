#define _POSIX_C_SOURCE 200809L

/*
 * Key trust: the ceremony that proves a node's public key is theirs, the verb that hands a key
 * back to the radio, and the vocabulary three screens read to say which of those has happened.
 *
 * These cases are unusually load-bearing for UI work, and for a reason worth stating once: the
 * failure mode of a verification feature is not a crash or a blank screen. It is a user being
 * told confidently that a key is proven when nothing proved it - which looks exactly like
 * working software, and which nothing downstream would ever notice. So the ceremony is tested
 * as a state machine with no radio anywhere near it, and the four rules that could produce that
 * failure each have a case of their own:
 *
 *   - a step never goes out addressed to a node nobody named (the nonce and node_num checks),
 *   - a "no" never sets the verified bit, and a failed send never sets it either,
 *   - a stage only asks the user something when the *radio* asked first,
 *   - the two sides of the seam agree about what a stage number means.
 */

#include "framework/mesh_test.h"
#include "support/session_fixture.h"

#include "mesh/core/key_verification.h"
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/store.h"
#include "mesh/ui/trust.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/admin.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ---- the vocabulary ------------------------------------------------------------------------ */

MESH_TEST_CASE(key_trust_reads_the_key_and_the_bit, unit) {
    struct mesh_ui_node_summary node;
    memset(&node, 0, sizeof node);

    MESH_TEST_FAIL_IF(mesh_ui_key_trust_of(NULL) != MESH_UI_KEY_TRUST_NONE,
                      "a node the roster has lost should read as no key");
    MESH_TEST_FAIL_IF(mesh_ui_key_trust_of(&node) != MESH_UI_KEY_TRUST_NONE,
                      "a node with no key should read as no key");

    /* The bit without a key is half a record. There is nothing to encrypt to, so the honest
       answer is the same one a node that has never sent a NodeInfo gets - not "verified". */
    node.key_verified = true;
    MESH_TEST_FAIL_IF(mesh_ui_key_trust_of(&node) != MESH_UI_KEY_TRUST_NONE,
                      "the verified bit with no key should still read as no key");

    node.public_key_len = 32U;
    node.public_key[0] = 0x42U;
    MESH_TEST_FAIL_IF(mesh_ui_key_trust_of(&node) != MESH_UI_KEY_TRUST_VERIFIED,
                      "a key with the bit set should read as verified");
    node.key_verified = false;
    MESH_TEST_FAIL_IF(mesh_ui_key_trust_of(&node) != MESH_UI_KEY_TRUST_UNVERIFIED,
                      "a key without the bit should read as unverified");
    record_success(test_name);
}

/*
 * The two states that carry a mark carry *different* marks, and both are sprites this build can
 * draw. A shared mark would be a transcript unable to tell "encrypted to a key" from "encrypted
 * to their key", and nothing in the renderer would notice - the same failure ui_delivery's
 * distinctness case exists to prevent, with more riding on it.
 */
MESH_TEST_CASE(key_trust_marks_are_distinct_and_drawable, unit) {
    const enum mesh_ui_icon none = mesh_ui_key_trust_icon(MESH_UI_KEY_TRUST_NONE);
    const enum mesh_ui_icon unverified = mesh_ui_key_trust_icon(MESH_UI_KEY_TRUST_UNVERIFIED);
    const enum mesh_ui_icon verified = mesh_ui_key_trust_icon(MESH_UI_KEY_TRUST_VERIFIED);

    MESH_TEST_FAIL_IF(mesh_ui_icon_is_valid(none),
                      "no key should carry no mark; there is nothing to draw a padlock about");
    MESH_TEST_FAIL_IF(!mesh_ui_icon_is_valid(unverified), "an unverified key has no mark");
    MESH_TEST_FAIL_IF(!mesh_ui_icon_is_valid(verified), "a verified key has no mark");
    MESH_TEST_FAIL_IF(unverified == verified,
                      "verified and unverified share a mark, so the padlock cannot tell them "
                      "apart");
    /* Sprites this build can find, rather than ids past the end of the generated table - which
       is what an icons.def edit committed without rerunning gen-icons.py leaves behind. */
    MESH_TEST_FAIL_IF(mesh_ui_icon_name(unverified)[0] == '\0',
                      "the unverified mark names a sprite this build cannot draw");
    MESH_TEST_FAIL_IF(mesh_ui_icon_name(verified)[0] == '\0',
                      "the verified mark names a sprite this build cannot draw");

    for (int trust = MESH_UI_KEY_TRUST_NONE; trust <= MESH_UI_KEY_TRUST_VERIFIED; ++trust) {
        char detail[96];
        snprintf(detail, sizeof detail, "trust %d has no word", trust);
        MESH_TEST_FAIL_IF(
            mesh_str(mesh_ui_key_trust_label((enum mesh_ui_key_trust)trust))[0] == '\0', detail);
    }
    /* An unverified key is the ordinary case on a working mesh. Colouring it would teach the
       user to ignore the colour by the end of the first day, which is the judgement in
       mesh/ui/trust.h and the one thing about that table worth pinning. */
    MESH_TEST_FAIL_IF(mesh_ui_key_trust_tone(MESH_UI_KEY_TRUST_UNVERIFIED) != MESH_UI_TONE_NORMAL,
                      "an unverified key should be drawn as the ordinary case");
    MESH_TEST_FAIL_IF(mesh_ui_key_trust_tone(MESH_UI_KEY_TRUST_VERIFIED) == MESH_UI_TONE_NORMAL,
                      "a verified key should be drawn as the thing somebody did");
    record_success(test_name);
}

/*
 * The UI restates the ceremony's limits and its stages rather than including the core header,
 * so that a backend can be built against a snapshot alone (include/mesh/ui/store.h). This is
 * what keeps the two copies honest - the same check the waypoint and network-host limits get.
 */
MESH_TEST_CASE(key_trust_limits_agree_across_the_seam, unit) {
    MESH_TEST_FAIL_IF(MESH_UI_VERIFY_NAME_MAX != MESH_KEY_VERIFICATION_NAME_MAX,
                      "the name limits disagree across the seam");
    MESH_TEST_FAIL_IF(MESH_UI_VERIFY_CHARS_MAX != MESH_KEY_VERIFICATION_CHARS_MAX,
                      "the verification-character limits disagree across the seam");
    MESH_TEST_FAIL_IF(MESH_UI_VERIFY_DIGITS != MESH_KEY_VERIFICATION_DIGITS,
                      "the digit counts disagree across the seam");
    MESH_TEST_FAIL_IF(MESH_UI_VERIFY_DIGITS_MAX != MESH_KEY_VERIFICATION_DIGITS,
                      "the keyboard's digit cap disagrees with the ceremony's");

    /* Value for value, because the stage crosses the seam as a byte: a renumbering on one side
       would put the wrong question in front of the user rather than failing to compile. */
    MESH_TEST_FAIL_IF(
        (int)MESH_UI_VERIFY_IDLE != (int)MESH_KEY_VERIFICATION_IDLE ||
            (int)MESH_UI_VERIFY_WAITING != (int)MESH_KEY_VERIFICATION_WAITING ||
            (int)MESH_UI_VERIFY_SHOW_NUMBER != (int)MESH_KEY_VERIFICATION_SHOW_NUMBER ||
            (int)MESH_UI_VERIFY_ENTER_NUMBER != (int)MESH_KEY_VERIFICATION_ENTER_NUMBER ||
            (int)MESH_UI_VERIFY_COMPARE != (int)MESH_KEY_VERIFICATION_COMPARE,
        "the stage enums disagree across the seam");
    record_success(test_name);
}

/* ---- the ceremony, with no radio anywhere near it ------------------------------------------- */

#define VERIFY_NOW 1750000000U

MESH_TEST_CASE(key_verification_initiator_walks_the_ceremony, unit) {
    struct mesh_key_verification state;
    mesh_key_verification_reset(&state);
    MESH_TEST_FAIL_IF(mesh_key_verification_active(&state), "a fresh record claimed an exchange");

    MESH_TEST_FAIL_IF(!mesh_key_verification_begin(&state, 0x2001U, "Pine Ridge", VERIFY_NOW),
                      "begin refused a real node");
    MESH_TEST_FAIL_IF(state.stage != (uint8_t)MESH_KEY_VERIFICATION_WAITING,
                      "the press should leave the exchange waiting on the radios");
    MESH_TEST_FAIL_IF(!state.we_initiated, "the end that pressed should be the initiator");
    MESH_TEST_FAIL_IF(state.nonce != 0U,
                      "the nonce is the radio's and does not exist until it answers");
    /* Waiting is live and asks nothing: there is no question to put in front of anybody until
       the radio comes back, which is why the sheet's first stage has no answer to give. */
    MESH_TEST_FAIL_IF(mesh_key_verification_asks(&state),
                      "the waiting stage should not be asking the user anything");

    MESH_TEST_FAIL_IF(
        !mesh_key_verification_on_number_request(&state, 0xABCDEFU, "Pine Ridge", VERIFY_NOW + 2U),
        "the request for a number did not move the stage");
    MESH_TEST_FAIL_IF(state.stage != (uint8_t)MESH_KEY_VERIFICATION_ENTER_NUMBER,
                      "the initiator should be asked to type the number");
    MESH_TEST_FAIL_IF(state.nonce != 0xABCDEFU, "the radio's nonce was not adopted");
    MESH_TEST_FAIL_IF(state.remote_node != 0x2001U,
                      "the node the user pressed on was lost when the nonce arrived");
    MESH_TEST_FAIL_IF(!mesh_key_verification_asks(&state), "the number stage asks nothing");

    MESH_TEST_FAIL_IF(
        !mesh_key_verification_on_final(&state, 0xABCDEFU, "Pine Ridge", "A7K2", VERIFY_NOW + 9U),
        "the final did not move the stage");
    MESH_TEST_FAIL_IF(state.stage != (uint8_t)MESH_KEY_VERIFICATION_COMPARE,
                      "both ends compare characters at the end");
    MESH_TEST_FAIL_IF(strcmp(state.characters, "A7K2") != 0, "the characters did not survive");
    MESH_TEST_FAIL_IF(!state.we_initiated,
                      "the final must not forget which end of the ceremony we are");
    /* The number has been read out and typed by now; two things to compare where there is one
       is how a user ends up comparing the wrong one. */
    MESH_TEST_FAIL_IF(state.security_number != 0U,
                      "the security number should not still be on the comparison sheet");

    struct mesh_key_verification done;
    MESH_TEST_FAIL_IF(!mesh_key_verification_settle(&state, &done), "settle refused a live one");
    MESH_TEST_FAIL_IF(done.remote_node != 0x2001U || done.nonce != 0xABCDEFU,
                      "settle did not hand back the exchange it ended");
    MESH_TEST_FAIL_IF(mesh_key_verification_active(&state), "settle left the exchange live");
    MESH_TEST_FAIL_IF(mesh_key_verification_settle(&state, NULL), "settle took an idle record");
    record_success(test_name);
}

/*
 * The other end, which never pressed anything: the ceremony arrives unannounced, and the client
 * has to be able to take part in one it did not start. Without this the Brick could only ever
 * verify keys it initiated, which halves a feature that takes two people either way.
 */
MESH_TEST_CASE(key_verification_responder_starts_cold, unit) {
    struct mesh_key_verification state;
    mesh_key_verification_reset(&state);

    MESH_TEST_FAIL_IF(
        !mesh_key_verification_on_number_inform(&state, 0x55U, "Fox Creek", 1234U, VERIFY_NOW),
        "an unannounced inform did not open an exchange");
    MESH_TEST_FAIL_IF(state.stage != (uint8_t)MESH_KEY_VERIFICATION_SHOW_NUMBER,
                      "the responder should be shown the number to read out");
    MESH_TEST_FAIL_IF(state.we_initiated, "the end that was told the number did not start it");
    MESH_TEST_FAIL_IF(state.security_number != 1234U, "the number to read out was lost");
    MESH_TEST_FAIL_IF(strcmp(state.remote_name, "Fox Creek") != 0, "the name was lost");
    /* Showing a number asks nothing: the user reads it out, and the exchange moves on when the
       other end types it. Nothing on this sheet is an answer. */
    MESH_TEST_FAIL_IF(mesh_key_verification_asks(&state),
                      "showing the number should not be asking the user anything");
    /* And the node number is the one thing an arrival never carries - the notifications name
       the far end by long name only, which is why the session resolves it against the roster. */
    MESH_TEST_FAIL_IF(state.remote_node != 0U,
                      "an unannounced exchange cannot know the node number from the wire");
    record_success(test_name);
}

/* The radio runs one exchange at a time, so an arrival naming a different nonce is the radio
   having moved on rather than a message to drop. A sheet showing the previous exchange would be
   asking about something nothing is waiting for. */
MESH_TEST_CASE(key_verification_adopts_a_replacing_exchange, unit) {
    struct mesh_key_verification state;
    mesh_key_verification_reset(&state);
    (void)mesh_key_verification_on_number_inform(&state, 0x11U, "Fox Creek", 1111U, VERIFY_NOW);

    (void)mesh_key_verification_on_final(&state, 0x22U, "Elk Pass", "ZZ99", VERIFY_NOW + 1U);
    MESH_TEST_FAIL_IF(state.nonce != 0x22U, "the newer exchange did not take the slot");
    MESH_TEST_FAIL_IF(strcmp(state.remote_name, "Elk Pass") != 0,
                      "the replaced exchange's name is still on the sheet");
    MESH_TEST_FAIL_IF(state.security_number != 0U,
                      "the replaced exchange's number is still on the sheet");
    record_success(test_name);
}

/*
 * Nothing on the wire ends an exchange, so the clock is the only thing that can - and a client
 * that cannot read a clock must not guess, because the cost of guessing is a question taken
 * away mid-answer.
 */
MESH_TEST_CASE(key_verification_expires_only_with_a_credible_clock, unit) {
    struct mesh_key_verification state;
    mesh_key_verification_reset(&state);
    (void)mesh_key_verification_begin(&state, 0x2001U, "Pine Ridge", VERIFY_NOW);

    MESH_TEST_FAIL_IF(mesh_key_verification_tick(&state, VERIFY_NOW + 1U, NULL),
                      "a one-second-old exchange was expired");
    MESH_TEST_FAIL_IF(mesh_key_verification_tick(&state, 0U, NULL),
                      "a client with no clock expired an exchange anyway");
    MESH_TEST_FAIL_IF(mesh_key_verification_tick(&state, VERIFY_NOW - 60U, NULL),
                      "a clock that went backwards expired an exchange");

    struct mesh_key_verification expired;
    MESH_TEST_FAIL_IF(!mesh_key_verification_tick(
                          &state, VERIFY_NOW + MESH_KEY_VERIFICATION_TIMEOUT_SECONDS, &expired),
                      "an exchange nothing moved for the timeout was not given up on");
    MESH_TEST_FAIL_IF(expired.remote_node != 0x2001U,
                      "the expiry did not say which node it was about");
    MESH_TEST_FAIL_IF(mesh_key_verification_active(&state), "the expired exchange is still live");

    /* An exchange the radio is still moving never ages out: the deadline runs from the last
       thing that happened, not from the press. */
    mesh_key_verification_reset(&state);
    (void)mesh_key_verification_begin(&state, 0x2001U, "Pine Ridge", VERIFY_NOW);
    (void)mesh_key_verification_on_number_request(
        &state, 0x9U, "Pine Ridge", VERIFY_NOW + MESH_KEY_VERIFICATION_TIMEOUT_SECONDS - 1U);
    MESH_TEST_FAIL_IF(mesh_key_verification_tick(
                          &state, VERIFY_NOW + MESH_KEY_VERIFICATION_TIMEOUT_SECONDS, NULL),
                      "an exchange that moved a second ago was expired on the press's clock");
    record_success(test_name);
}

/* ---- the two admin verbs -------------------------------------------------------------------- */

/* Encodes one request and hands back the AdminMessage inside it, or false if the encoder
   refused. Refusals are the point of half these cases, so the two are told apart by the
   caller. */
static bool key_trust_encodes(const struct mesh_admin_request *request,
                              meshtastic_AdminMessage *out_admin) {
    uint8_t buffer[512];
    size_t written = 0U;
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);
    if (mesh_radio_settings_encode_request(&settings, request, buffer, sizeof buffer, &written) !=
        0) {
        return false;
    }
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio) ||
        to_radio.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP) {
        return false;
    }
    *out_admin = (meshtastic_AdminMessage)meshtastic_AdminMessage_init_default;
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    return pb_decode(&in, meshtastic_AdminMessage_fields, out_admin);
}

static struct mesh_admin_request key_trust_contact_request(void) {
    struct mesh_admin_request request;
    memset(&request, 0, sizeof request);
    request.kind = MESH_ADMIN_ADD_CONTACT;
    request.type = 0x2001U;
    request.my_node = 0x1234U;
    request.packet_id = 90U;
    request.payload.contact.node_num = 0x2001U;
    request.payload.contact.has_user = true;
    snprintf(request.payload.contact.user.id, sizeof request.payload.contact.user.id, "!00002001");
    snprintf(request.payload.contact.user.long_name, sizeof request.payload.contact.user.long_name,
             "Pine Ridge");
    request.payload.contact.user.public_key.size = 32U;
    request.payload.contact.user.public_key.bytes[0] = 0xA1U;
    request.payload.contact.manually_verified = true;
    return request;
}

MESH_TEST_CASE(key_trust_add_contact_encodes, unit) {
    const struct mesh_admin_request request = key_trust_contact_request();
    meshtastic_AdminMessage admin;
    MESH_TEST_FAIL_IF(!key_trust_encodes(&request, &admin), "add_contact would not encode");
    MESH_TEST_FAIL_IF(admin.which_payload_variant != meshtastic_AdminMessage_add_contact_tag,
                      "add_contact encoded as something else");
    MESH_TEST_FAIL_IF(admin.add_contact.node_num != 0x2001U, "the node number did not survive");
    MESH_TEST_FAIL_IF(admin.add_contact.user.public_key.size != 32U ||
                          admin.add_contact.user.public_key.bytes[0] != 0xA1U,
                      "the public key did not survive, which is the whole of this verb");
    MESH_TEST_FAIL_IF(!admin.add_contact.manually_verified,
                      "a key proven out of band arrived at the radio as a stranger's");
    record_success(test_name);
}

MESH_TEST_CASE(key_trust_add_contact_refuses_what_the_radio_would_not_want, unit) {
    struct mesh_admin_request request = key_trust_contact_request();
    meshtastic_AdminMessage admin;

    /* No key: the radio would build exactly this entry for itself the moment the node
       transmitted, so writing one spends a NodeDB slot to say nothing. */
    request.payload.contact.user.public_key.size = 0U;
    MESH_TEST_FAIL_IF(key_trust_encodes(&request, &admin), "a contact with no key was encoded");

    /* The node number filed under one id and written under another. The queue dedupes on
       `type`, so a mismatch here would file one node's contact under another node's slot. */
    request = key_trust_contact_request();
    request.type = 0x2002U;
    MESH_TEST_FAIL_IF(key_trust_encodes(&request, &admin),
                      "a contact whose type and node number disagree was encoded");

    request = key_trust_contact_request();
    request.payload.contact.has_user = false;
    MESH_TEST_FAIL_IF(key_trust_encodes(&request, &admin), "a contact with no User was encoded");
    record_success(test_name);
}

MESH_TEST_CASE(key_trust_verification_steps_encode, unit) {
    struct mesh_admin_request request;
    memset(&request, 0, sizeof request);
    request.kind = MESH_ADMIN_KEY_VERIFICATION;
    request.type = 0x2001U;
    request.my_node = 0x1234U;
    request.packet_id = 91U;
    request.payload.key_verification.message_type =
        meshtastic_KeyVerificationAdmin_MessageType_INITIATE_VERIFICATION;
    request.payload.key_verification.remote_nodenum = 0x2001U;

    meshtastic_AdminMessage admin;
    MESH_TEST_FAIL_IF(!key_trust_encodes(&request, &admin), "the opening step would not encode");
    MESH_TEST_FAIL_IF(admin.which_payload_variant != meshtastic_AdminMessage_key_verification_tag,
                      "the step encoded as something else");
    MESH_TEST_FAIL_IF(admin.key_verification.has_security_number,
                      "a step that carries no digits claimed a security number");

    /* Every step after the first quotes the nonce the radio opened the exchange with. One that
       does not is answering an exchange that is not the one in front of the user, which is
       exactly what the nonce is there to prevent - so it is refused here rather than sent. */
    request.payload.key_verification.message_type =
        meshtastic_KeyVerificationAdmin_MessageType_DO_VERIFY;
    MESH_TEST_FAIL_IF(key_trust_encodes(&request, &admin), "a DO_VERIFY with no nonce was encoded");

    request.payload.key_verification.nonce = 0xABCDEFU;
    MESH_TEST_FAIL_IF(!key_trust_encodes(&request, &admin), "a DO_VERIFY with a nonce was refused");
    MESH_TEST_FAIL_IF(admin.key_verification.nonce != 0xABCDEFU, "the nonce did not survive");

    request.payload.key_verification.message_type =
        meshtastic_KeyVerificationAdmin_MessageType_PROVIDE_SECURITY_NUMBER;
    request.payload.key_verification.has_security_number = true;
    request.payload.key_verification.security_number = 4321U;
    MESH_TEST_FAIL_IF(!key_trust_encodes(&request, &admin), "the number step was refused");
    MESH_TEST_FAIL_IF(!admin.key_verification.has_security_number ||
                          admin.key_verification.security_number != 4321U,
                      "the security number did not survive");

    /* Addressed to nobody: the session refuses these before they get here, and so does this. */
    request.payload.key_verification.remote_nodenum = 0U;
    MESH_TEST_FAIL_IF(key_trust_encodes(&request, &admin),
                      "a step addressed to no node was encoded");
    record_success(test_name);
}

/* ---- the session verbs ---------------------------------------------------------------------- */

/* A session with a radio attached and one node in the roster carrying a public key. The send
   path throws the bytes away: what these cases are about is what the session records and what
   it refuses, not what reaches a radio. */
static int key_trust_sink(void *ctx, const uint8_t *packet, size_t len, uint32_t packet_id) {
    (void)packet;
    (void)len;
    (void)packet_id;
    if (ctx != NULL) {
        *(unsigned *)ctx += 1U;
    }
    return 0;
}

static void key_trust_seed(struct mesh_session *session, unsigned *sends, bool verified) {
    mesh_session_init(session);
    mesh_session_attach(session, key_trust_sink, sends);

    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = 0x1000U;
    (void)mesh_test_session_feed_from_radio(session, &my_info);

    meshtastic_FromRadio info = meshtastic_FromRadio_init_default;
    info.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    info.node_info.num = 0x2001U;
    info.node_info.has_user = true;
    snprintf(info.node_info.user.id, sizeof info.node_info.user.id, "!00002001");
    snprintf(info.node_info.user.long_name, sizeof info.node_info.user.long_name, "Pine Ridge");
    snprintf(info.node_info.user.short_name, sizeof info.node_info.user.short_name, "PINE");
    info.node_info.user.public_key.size = 32U;
    info.node_info.user.public_key.bytes[0] = 0xA1U;
    info.node_info.is_key_manually_verified = verified;
    (void)mesh_test_session_feed_from_radio(session, &info);
}

/* The radio is the authority on the verified bit, the way it is on favourite, ignored and
   muted: a ceremony run from a phone against the same radio is the case where our own copy is
   the stale one. */
MESH_TEST_CASE(key_trust_session_keeps_the_verified_bit, unit) {
    struct mesh_session session;
    unsigned sends = 0U;
    key_trust_seed(&session, &sends, true);

    const struct mesh_node_summary *node = mesh_test_session_find_node(&session, 0x2001U);
    MESH_TEST_FAIL_IF(node == NULL, "the seeded node is not in the roster");
    MESH_TEST_FAIL_IF(!node->key_verified, "the radio's verified bit was dropped on the way in");
    MESH_TEST_FAIL_IF(node->public_key_len != 32U, "the public key was dropped on the way in");

    /* And it comes back off again when the radio says so - the bit is the radio's, not a
       preference this client accumulates. */
    key_trust_seed(&session, &sends, false);
    node = mesh_test_session_find_node(&session, 0x2001U);
    MESH_TEST_FAIL_IF(node == NULL || node->key_verified,
                      "a NodeInfo without the bit left the node reading as verified");
    record_success(test_name);
}

MESH_TEST_CASE(key_trust_session_add_contact_carries_the_record, unit) {
    struct mesh_session session;
    unsigned sends = 0U;
    key_trust_seed(&session, &sends, true);

    MESH_TEST_FAIL_IF(mesh_session_add_contact(&session, 0x1000U) != -EINVAL,
                      "our own node was offered to the radio it already is");
    MESH_TEST_FAIL_IF(mesh_session_add_contact(&session, 0x3003U) != -ENOENT,
                      "a node the roster does not have was accepted");

    const int queued = mesh_session_add_contact(&session, 0x2001U);
    MESH_TEST_FAIL_IF(queued <= 0, "a node with a key was not queued");

    /* The passkey refresh and the verb, which is the shape every NodeDB press takes: the
       firmware rejects a write whose session key has aged out, and the one we hold may be
       minutes old. */
    struct mesh_admin_request first;
    struct mesh_admin_request second;
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&session.settings, 1000U, &first),
                      "nothing was queued");
    MESH_TEST_FAIL_IF(first.kind != MESH_ADMIN_GET_OWNER,
                      "the contact did not go out behind a passkey refresh");
    mesh_radio_settings_mark_sent(&session.settings, first.packet_id, 1000U);
    session.settings.pending_request_id = 0U;
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&session.settings, 1001U, &second),
                      "the contact itself was not queued");
    MESH_TEST_FAIL_IF(second.kind != MESH_ADMIN_ADD_CONTACT, "the second request is not the verb");
    MESH_TEST_FAIL_IF(second.payload.contact.node_num != 0x2001U ||
                          second.payload.contact.user.public_key.size != 32U,
                      "the contact did not carry the roster's key");
    MESH_TEST_FAIL_IF(!second.payload.contact.manually_verified,
                      "a verified node was handed back to the radio as a stranger");
    MESH_TEST_FAIL_IF(strcmp(second.payload.contact.user.long_name, "Pine Ridge") != 0,
                      "the contact did not carry the roster's name");
    /* An add-contact must not be counted as a settings write: the Settings tab would announce
       an unsaved section in flight for a press on the Nodes tab. */
    MESH_TEST_FAIL_IF(mesh_admin_request_is_write(MESH_ADMIN_ADD_CONTACT),
                      "add_contact counts as a settings write");
    MESH_TEST_FAIL_IF(mesh_admin_request_is_write(MESH_ADMIN_KEY_VERIFICATION),
                      "a verification step counts as a settings write");
    record_success(test_name);
}

/*
 * The one rule that matters most in this file: a "no" never marks a key verified, and a key
 * with no cached bit stays without one. Getting this backwards is the failure that looks like
 * working software.
 */
MESH_TEST_CASE(key_trust_session_settle_only_a_yes_marks_the_key, unit) {
    struct mesh_session session;
    unsigned sends = 0U;
    key_trust_seed(&session, &sends, false);

    MESH_TEST_FAIL_IF(mesh_session_verify_key_settle(&session, true) != -EINVAL,
                      "an answer was accepted with no exchange open");

    /* A refusal, through the whole ceremony. */
    MESH_TEST_FAIL_IF(mesh_session_verify_key_begin(&session, 0x2001U) <= 0,
                      "the ceremony would not start against a node with a key");
    (void)mesh_key_verification_on_final(&session.verification, 0x77U, "Pine Ridge", "A7K2",
                                         VERIFY_NOW);
    MESH_TEST_FAIL_IF(mesh_session_verify_key_settle(&session, false) < 0,
                      "a refusal was not accepted");
    const struct mesh_node_summary *node = mesh_test_session_find_node(&session, 0x2001U);
    MESH_TEST_FAIL_IF(node == NULL || node->key_verified,
                      "answering that the codes do not match marked the key verified");
    MESH_TEST_FAIL_IF(mesh_key_verification_active(mesh_session_verification(&session)),
                      "a refusal left the exchange open");

    /* The first ceremony's steps are still sitting in the queue - nothing here drains it - and
       an identical step for the same node is refused as a double press, which is the guard
       doing its job rather than a problem with the case. Clearing the admin session is what a
       radio that had answered them would have left behind. */
    MESH_TEST_FAIL_IF(mesh_session_verify_key_begin(&session, 0x2001U) != -EBUSY,
                      "an INITIATE already waiting to go out was queued a second time");
    mesh_radio_settings_reset_session(&session.settings);

    /* And a yes, which does mark it. The radio's own answer is this node's next NodeInfo, hours
       away on a quiet mesh - a row still reading "not verified" after the ceremony the user has
       just finished would be the client disagreeing with itself about something it just did. */
    MESH_TEST_FAIL_IF(mesh_session_verify_key_begin(&session, 0x2001U) <= 0,
                      "a second ceremony would not start");
    (void)mesh_key_verification_on_final(&session.verification, 0x78U, "Pine Ridge", "A7K2",
                                         VERIFY_NOW);
    MESH_TEST_FAIL_IF(mesh_session_verify_key_settle(&session, true) < 0, "a yes was not accepted");
    node = mesh_test_session_find_node(&session, 0x2001U);
    MESH_TEST_FAIL_IF(node == NULL || !node->key_verified,
                      "a completed verification did not mark the key");
    record_success(test_name);
}

/* Nothing to verify about a key we do not hold: the ceremony would run to the end and prove
   it. Refused at the session, where the roster is, rather than discovered five minutes and two
   phone calls later. */
MESH_TEST_CASE(key_trust_session_refuses_a_node_with_no_key, unit) {
    struct mesh_session session;
    unsigned sends = 0U;
    key_trust_seed(&session, &sends, false);

    meshtastic_FromRadio info = meshtastic_FromRadio_init_default;
    info.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    info.node_info.num = 0x2002U;
    info.node_info.has_user = true;
    snprintf(info.node_info.user.long_name, sizeof info.node_info.user.long_name, "Keyless");
    (void)mesh_test_session_feed_from_radio(&session, &info);

    MESH_TEST_FAIL_IF(mesh_session_verify_key_begin(&session, 0x2002U) != -EINVAL,
                      "a ceremony was started against a node we hold no key for");
    MESH_TEST_FAIL_IF(mesh_session_add_contact(&session, 0x2002U) != -EINVAL,
                      "a keyless node was offered to the radio");
    MESH_TEST_FAIL_IF(mesh_session_verify_key_begin(&session, 0x1000U) != -EINVAL,
                      "a ceremony was started against our own radio");
    record_success(test_name);
}

/*
 * The ClientNotification path: the radio's only way of asking its user something, and the one
 * route into the ceremony that is not a press. A build that folded the text in and dropped the
 * payload variant would look entirely correct - the Status row would carry the firmware's
 * sentence - and no verification would ever be answerable.
 */
MESH_TEST_CASE(key_trust_session_reads_the_radio_asking, unit) {
    struct mesh_session session;
    unsigned sends = 0U;
    key_trust_seed(&session, &sends, false);

    meshtastic_FromRadio note = meshtastic_FromRadio_init_default;
    note.which_payload_variant = meshtastic_FromRadio_clientNotification_tag;
    note.clientNotification.level = meshtastic_LogRecord_Level_INFO;
    snprintf(note.clientNotification.message, sizeof note.clientNotification.message,
             "Verification code from Pine Ridge");
    note.clientNotification.which_payload_variant =
        meshtastic_ClientNotification_key_verification_number_inform_tag;
    note.clientNotification.payload_variant.key_verification_number_inform.nonce = 0x99U;
    note.clientNotification.payload_variant.key_verification_number_inform.security_number = 8642U;
    snprintf(note.clientNotification.payload_variant.key_verification_number_inform.remote_longname,
             sizeof note.clientNotification.payload_variant.key_verification_number_inform
                 .remote_longname,
             "Pine Ridge");
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &note),
                      "encode the notification failed");

    const struct mesh_key_verification *live = mesh_session_verification(&session);
    MESH_TEST_FAIL_IF(live->stage != (uint8_t)MESH_KEY_VERIFICATION_SHOW_NUMBER,
                      "the radio asking its user to read a number did not reach the ceremony");
    MESH_TEST_FAIL_IF(live->security_number != 8642U, "the number to read out did not arrive");
    /* Resolved against the roster, because no notification carries a node number - and without
       it there is nothing to address the answering step to. */
    MESH_TEST_FAIL_IF(live->remote_node != 0x2001U,
                      "the far end was not resolved to a node in the roster");
    /* The firmware's own sentence still reaches the Status row: folding the variant in must not
       cost the text beside it. */
    MESH_TEST_FAIL_IF(mesh_session_notification(&session)->text[0] == '\0',
                      "the notification's words were lost with its payload");

    /* A link that drops takes the exchange with it: every step is an AdminMessage to this
       radio, quoting a nonce this radio opened. */
    mesh_session_detach(&session);
    MESH_TEST_FAIL_IF(mesh_key_verification_active(mesh_session_verification(&session)),
                      "a dropped link left a ceremony standing");
    record_success(test_name);
}

/* ---- the sheet ------------------------------------------------------------------------------ */

/*
 * Every stage the sheet is asked to draw answers with two labels and something to say, and the
 * one stage that is not a sheet says so. A stage that fell through to an empty panel would be a
 * question with no way to answer it, on a screen the user cannot get out of without B.
 */
MESH_TEST_CASE(key_trust_sheet_answers_every_stage, unit) {
    const enum mesh_ui_verify_stage sheets[] = {
        MESH_UI_VERIFY_WAITING,
        MESH_UI_VERIFY_SHOW_NUMBER,
        MESH_UI_VERIFY_COMPARE,
    };
    for (size_t i = 0; i < sizeof sheets / sizeof sheets[0]; ++i) {
        struct mesh_ui_verification verification;
        memset(&verification, 0, sizeof verification);
        verification.stage = (uint8_t)sheets[i];
        verification.remote_node = 0x2001U;
        verification.security_number = 1234U;
        snprintf(verification.remote_name, sizeof verification.remote_name, "Pine Ridge");
        snprintf(verification.characters, sizeof verification.characters, "A7K2");

        struct mesh_ui_verify_sheet sheet;
        char headline[96];
        char text[256];
        char detail[96];
        snprintf(detail, sizeof detail, "stage %u has no sheet", (unsigned)sheets[i]);
        MESH_TEST_FAIL_IF(!mesh_ui_verify_sheet_of(&verification, &sheet, headline, sizeof headline,
                                                   text, sizeof text),
                          detail);
        snprintf(detail, sizeof detail, "stage %u has no headline", (unsigned)sheets[i]);
        MESH_TEST_FAIL_IF(headline[0] == '\0', detail);
        /* Every stage keeps a paragraph, and that is not decoration: what makes this ceremony
           proof of anything is that the number and the code travel by voice, and the paragraph
           is the only thing on the panel that says so. */
        snprintf(detail, sizeof detail, "stage %u has no explanation", (unsigned)sheets[i]);
        MESH_TEST_FAIL_IF(text[0] == '\0', detail);
        snprintf(detail, sizeof detail, "stage %u has an unlabelled answer", (unsigned)sheets[i]);
        MESH_TEST_FAIL_IF(mesh_str(sheet.accept)[0] == '\0' || mesh_str(sheet.cancel)[0] == '\0',
                          detail);
        MESH_TEST_FAIL_IF(sheet.accept == sheet.cancel, "a stage offers the same answer twice");
    }

    /* The comparison puts the characters themselves in the headline, because they are the thing
       being compared rather than a sentence about it. */
    struct mesh_ui_verification comparing;
    memset(&comparing, 0, sizeof comparing);
    comparing.stage = (uint8_t)MESH_UI_VERIFY_COMPARE;
    snprintf(comparing.characters, sizeof comparing.characters, "A7K2");
    snprintf(comparing.remote_name, sizeof comparing.remote_name, "Pine Ridge");
    struct mesh_ui_verify_sheet sheet;
    char headline[96];
    char text[256];
    MESH_TEST_FAIL_IF(
        !mesh_ui_verify_sheet_of(&comparing, &sheet, headline, sizeof headline, text, sizeof text),
        "the comparison has no sheet");
    MESH_TEST_FAIL_IF(strcmp(headline, "A7K2") != 0,
                      "the comparison sheet does not show the code being compared");

    /* Idle draws nothing, and so does the one stage the keyboard owns: a dialog whose only
       answer opened a keyboard would be a press in front of a press. */
    struct mesh_ui_verification idle;
    memset(&idle, 0, sizeof idle);
    MESH_TEST_FAIL_IF(
        mesh_ui_verify_sheet_of(&idle, &sheet, headline, sizeof headline, text, sizeof text),
        "an idle record produced a sheet");
    idle.stage = (uint8_t)MESH_UI_VERIFY_ENTER_NUMBER;
    MESH_TEST_FAIL_IF(
        mesh_ui_verify_sheet_of(&idle, &sheet, headline, sizeof headline, text, sizeof text),
        "the number stage produced a sheet as well as a keyboard");
    record_success(test_name);
}

/*
 * The node detail's key rows: the state always, and the two verbs only where they can do
 * something. A row that could only ever fail teaches the user to distrust the rows around it.
 */
MESH_TEST_CASE(key_trust_node_detail_offers_the_key_rows, unit) {
    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 1U;
    struct mesh_ui_node_summary *node = &handshake.nodes[0];
    node->node_id = 0x2001U;
    node->has_user = true;
    snprintf(node->long_name, sizeof node->long_name, "Pine Ridge");
    snprintf(node->short_name, sizeof node->short_name, "PINE");

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    struct {
        const char *label;
        bool has_key;
        bool verified;
        bool in_nodedb;
        bool expect_verify;
        bool expect_add;
        enum mesh_str_id expect_state;
    } cases[] = {
        {"no key", false, false, true, false, false, MESH_STR_TRUST_NONE},
        {"key, on the radio", true, false, true, true, false, MESH_STR_TRUST_UNVERIFIED},
        {"key, off the radio", true, false, false, true, true, MESH_STR_TRUST_UNVERIFIED},
        {"verified, off the radio", true, true, false, true, true, MESH_STR_TRUST_VERIFIED},
        /* A node with no key that the radio has also lost gets neither verb: there is nothing
           to hand over and nothing to prove. */
        {"no key, off the radio", false, false, false, false, false, MESH_STR_TRUST_NONE},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        node->public_key_len = cases[i].has_key ? 32U : 0U;
        node->public_key[0] = 0xA1U;
        node->key_verified = cases[i].verified;
        node->in_nodedb = cases[i].in_nodedb;

        const uint32_t count = mesh_ui_node_detail_build(node, false, 0U, NULL, false, &handshake,
                                                         NULL, items, MESH_UI_NODE_ITEMS_MAX);

        bool saw_verify = false;
        bool saw_add = false;
        bool saw_state = false;
        for (uint32_t row = 0; row < count; ++row) {
            const struct mesh_ui_node_item *item = &items[row];
            if (item->action == (uint8_t)MESH_UI_NODE_ACTION_VERIFY_KEY) {
                saw_verify = true;
            }
            if (item->action == (uint8_t)MESH_UI_NODE_ACTION_ADD_CONTACT) {
                saw_add = true;
            }
            if (item->kind == (uint8_t)MESH_UI_NODE_ROW_INFO &&
                strcmp(item->value, mesh_str(cases[i].expect_state)) == 0) {
                saw_state = true;
            }
        }
        char reason[128];
        snprintf(reason, sizeof reason, "%s: the key's state is not on the screen", cases[i].label);
        MESH_TEST_FAIL_IF(!saw_state, reason);
        snprintf(reason, sizeof reason, "%s: the verify row is %s", cases[i].label,
                 cases[i].expect_verify ? "missing" : "offered with nothing to verify");
        MESH_TEST_FAIL_IF(saw_verify != cases[i].expect_verify, reason);
        snprintf(reason, sizeof reason, "%s: the put-back row is %s", cases[i].label,
                 cases[i].expect_add ? "missing" : "offered for a node the radio already has");
        MESH_TEST_FAIL_IF(saw_add != cases[i].expect_add, reason);
    }
    record_success(test_name);
}

/* ---- the sheet's presses --------------------------------------------------------------------
 *
 * Which answer each button gives depends on the stage, and the stage lives in the store rather
 * than on the nav - so these go through mesh_ui_nav_handle_key() with a store standing behind
 * it, exactly as the frame does. What is being pinned is the mapping from a press to a verb,
 * because that is the mapping a mis-press turns into a refusal the user never made.
 */

static void key_trust_open_sheet(struct mesh_ui_store *store, struct mesh_ui_nav *nav,
                                 enum mesh_ui_verify_stage stage) {
    struct mesh_ui_verification verification;
    memset(&verification, 0, sizeof verification);
    verification.stage = (uint8_t)stage;
    verification.remote_node = 0x2001U;
    verification.security_number = 1234U;
    snprintf(verification.remote_name, sizeof verification.remote_name, "Pine Ridge");
    snprintf(verification.characters, sizeof verification.characters, "A7K2");
    mesh_ui_store_set_verification(store, &verification);
    mesh_ui_store_open_verify_sheet(store);
    *nav = store->nav;
}

MESH_TEST_CASE(key_trust_sheet_presses_answer_the_right_way, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_nav nav;
    struct mesh_ui_action action;

    /* The comparison: both answers are answers, and which one A gives is the cursor. */
    key_trust_open_sheet(&store, &nav, MESH_UI_VERIFY_COMPARE);
    MESH_TEST_FAIL_IF(!nav.verify_open, "the sheet did not open");
    MESH_TEST_FAIL_IF(nav.verify_cursor != 0U, "the sheet should open on the answer that acts");
    memset(&action, 0, sizeof action);
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(action.type != MESH_UI_ACTION_VERIFY_ANSWER || action.number != 1U,
                      "A on \"they match\" did not verify the key");
    MESH_TEST_FAIL_IF(nav.verify_open, "answering left the sheet up");

    key_trust_open_sheet(&store, &nav, MESH_UI_VERIFY_COMPARE);
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_DOWN, NULL);
    MESH_TEST_FAIL_IF(nav.verify_cursor != 1U, "the d-pad does not move between the two answers");
    memset(&action, 0, sizeof action);
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(action.type != MESH_UI_ACTION_VERIFY_ANSWER || action.number != 0U,
                      "A on \"they do not\" did not refuse the key");

    /*
     * B is not an answer, on any stage including this one. Backing out is what B does
     * everywhere else in the client, and a B that quietly told the far end the codes did not
     * match would turn a mis-press into a refusal the user never made. The exchange stays open
     * and the core expires it.
     */
    key_trust_open_sheet(&store, &nav, MESH_UI_VERIFY_COMPARE);
    memset(&action, 0, sizeof action);
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_B, &action);
    MESH_TEST_FAIL_IF(action.type != MESH_UI_ACTION_NONE, "B answered the comparison");
    MESH_TEST_FAIL_IF(nav.verify_open, "B did not close the sheet");

    /*
     * The two waiting stages: the first answer only gets out of the way, because the exchange
     * is still running and the sheet comes back when the radio next asks for something. Only
     * the second stands it down.
     */
    const enum mesh_ui_verify_stage waiting[] = {MESH_UI_VERIFY_WAITING,
                                                 MESH_UI_VERIFY_SHOW_NUMBER};
    for (size_t i = 0; i < sizeof waiting / sizeof waiting[0]; ++i) {
        char detail[96];
        key_trust_open_sheet(&store, &nav, waiting[i]);
        memset(&action, 0, sizeof action);
        (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_A, &action);
        snprintf(detail, sizeof detail,
                 "stage %u: getting out of the way told the radio "
                 "something",
                 (unsigned)waiting[i]);
        MESH_TEST_FAIL_IF(action.type != MESH_UI_ACTION_NONE, detail);
        snprintf(detail, sizeof detail, "stage %u: the sheet stayed up", (unsigned)waiting[i]);
        MESH_TEST_FAIL_IF(nav.verify_open, detail);

        key_trust_open_sheet(&store, &nav, waiting[i]);
        (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_RIGHT, NULL);
        memset(&action, 0, sizeof action);
        (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_A, &action);
        snprintf(detail, sizeof detail, "stage %u: stopping did not stand the ceremony down",
                 (unsigned)waiting[i]);
        MESH_TEST_FAIL_IF(action.type != MESH_UI_ACTION_VERIFY_ANSWER || action.number != 0U,
                          detail);
    }

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * The security number's keyboard: four digits and nothing else.
 *
 * Short of four is refused where it is typed rather than sent. A pairing PIN is the opposite
 * case - a wrong answer there costs another thirty seconds of BlueZ - while a half-typed
 * security number fails the *verification*, and the two people have to start again from the
 * beginning with a phone call in between.
 */
MESH_TEST_CASE(key_trust_number_keyboard_takes_four_digits, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_verification verification;
    memset(&verification, 0, sizeof verification);
    verification.stage = (uint8_t)MESH_UI_VERIFY_ENTER_NUMBER;
    verification.remote_node = 0x2001U;
    snprintf(verification.remote_name, sizeof verification.remote_name, "Pine Ridge");
    mesh_ui_store_set_verification(&store, &verification);
    mesh_ui_store_open_verify_number(&store);

    struct mesh_ui_nav nav = store.nav;
    MESH_TEST_FAIL_IF(!nav.keyboard_open || !nav.keyboard_verify,
                      "the number prompt did not take the keyboard");
    MESH_TEST_FAIL_IF(mesh_ui_nav_draft_cap(&nav) != MESH_UI_VERIFY_DIGITS_MAX,
                      "the prompt would take more digits than the radio generates");

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    snprintf(nav.draft, sizeof nav.draft, "123");
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_START, &action);
    MESH_TEST_FAIL_IF(action.type != MESH_UI_ACTION_NONE, "three digits were sent as a number");
    MESH_TEST_FAIL_IF(!nav.keyboard_open, "a refused number closed the prompt anyway");

    memset(&action, 0, sizeof action);
    snprintf(nav.draft, sizeof nav.draft, "1234");
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_START, &action);
    MESH_TEST_FAIL_IF(action.type != MESH_UI_ACTION_VERIFY_NUMBER, "four digits were not sent");
    MESH_TEST_FAIL_IF(strcmp(action.text, "1234") != 0, "the digits did not survive the press");
    MESH_TEST_FAIL_IF(nav.keyboard_open, "the prompt stayed up after being answered");

    /* Backing out of the prompt stands the ceremony down rather than leaving it open: the other
       person is holding a number up waiting, and the honest reading of "I am not typing that"
       is that they can stop. */
    mesh_ui_store_open_verify_number(&store);
    nav = store.nav;
    memset(&action, 0, sizeof action);
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_B, &action);
    MESH_TEST_FAIL_IF(action.type != MESH_UI_ACTION_VERIFY_ANSWER || action.number != 0U,
                      "backing out of the number prompt left the other end waiting");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * Every action label on the node detail fits the row it is written into, in every language the
 * build ships.
 *
 * `MESH_UI_NODE_LABEL_MAX` is 20 bytes including the NUL, and `rows_action()` copies into it
 * with snprintf - so a label a character too long is *cut*, with nothing on the frame saying so.
 * That is the same fault the settings text buffer was rewritten to end: a mistake invisible
 * until somebody reads the screen, and only in the locale nobody on the team reads. It arrived
 * the moment this feature did - "Put back on the radio" drew as "Put back on the rad" - and the
 * only reason it was caught is that somebody looked at a picture.
 *
 * The list is this screen's verbs rather than the whole catalog, because this cap belongs to
 * this row model; a label elsewhere is measured against its own.
 */
MESH_TEST_CASE(key_trust_node_action_labels_fit_their_row, unit) {
    static const enum mesh_str_id kLabels[] = {
        MESH_STR_NODE_ACT_MESSAGE,       MESH_STR_NODE_ACT_PIN,
        MESH_STR_NODE_ACT_REQUEST_INFO,  MESH_STR_NODE_ACT_REQUEST_POSITION,
        MESH_STR_NODE_ACT_REQUEST_TELEM, MESH_STR_NODE_ACT_MUTE,
        MESH_STR_NODE_ACT_IGNORE,        MESH_STR_NODE_ACT_REMOVE,
        MESH_STR_NODE_ACT_WAYPOINT,      MESH_STR_NODE_ACT_SHOW_ON_MAP,
        MESH_STR_NODE_ACT_VERIFY_KEY,    MESH_STR_NODE_ACT_VERIFY_AGAIN,
        MESH_STR_NODE_ACT_ADD_CONTACT,   MESH_STR_NODE_KEY_TRUST,
    };
    for (size_t locale = 0; locale < mesh_i18n_locale_count(); ++locale) {
        const struct mesh_i18n_locale *const which = mesh_i18n_locale_at(locale);
        for (size_t i = 0; i < sizeof kLabels / sizeof kLabels[0]; ++i) {
            const char *const text = mesh_str_in(which, kLabels[i]);
            if (strlen(text) < MESH_UI_NODE_LABEL_MAX) {
                continue;
            }
            /* Named in both dimensions, because "a label is too long" over two locales and
               fourteen verbs is a bisect rather than a failure message. */
            char reason[160];
            snprintf(reason, sizeof reason, "%s: %s is %u bytes, over the %u-byte row label",
                     which->id, mesh_str_id_name(kLabels[i]), (unsigned)strlen(text),
                     (unsigned)MESH_UI_NODE_LABEL_MAX - 1U);
            record_failure(test_name, reason);
            return;
        }
    }
    record_success(test_name);
}

/* ---- four things a review caught ------------------------------------------------------------
 *
 * Each of these is a case that failed before the fix beside it. They are grouped because they
 * are one class of mistake: the ceremony has two clocks, two peers and two overlays in play at
 * once, and every one of these was a place where the code kept the wrong half of a pair.
 */

/*
 * An exchange replaced by one for a *different* peer must not keep the old peer's node number.
 *
 * This is the worst bug the feature could have had, and it looked entirely reasonable: the slot
 * kept its node so that our own WAITING exchange would not lose the node the user pressed on
 * when the radio's nonce arrived. But a replacement carries a new name and no node number, so
 * the sheet showed the new peer while every step - and the optimistic verified bit behind a
 * yes - went to the old one. A key marked proven from a code that belonged to somebody else is
 * exactly what this whole feature exists to prevent.
 *
 * The two cases are told apart by the slot's nonce: 0 is our own initiation acquiring one,
 * anything else is a different exchange.
 */
MESH_TEST_CASE(key_verification_replacement_does_not_inherit_the_peer, unit) {
    struct mesh_key_verification state;

    /* Our own initiation gaining its nonce: the node survives, or the ceremony has nothing to
       address. */
    mesh_key_verification_reset(&state);
    (void)mesh_key_verification_begin(&state, 0x2001U, "Pine Ridge", VERIFY_NOW);
    (void)mesh_key_verification_on_number_request(&state, 0xAAU, "Pine Ridge", VERIFY_NOW + 1U);
    MESH_TEST_FAIL_IF(state.remote_node != 0x2001U,
                      "the node the user pressed on was lost when the radio's nonce arrived");

    /* A different exchange replacing a resolved one: the node must go with it, so the session
       resolves the new name rather than addressing the old node. */
    mesh_key_verification_reset(&state);
    (void)mesh_key_verification_on_number_inform(&state, 0x11U, "Fox Creek", 1111U, VERIFY_NOW);
    state.remote_node = 0x2002U; /* as the session's name resolution would have left it */
    (void)mesh_key_verification_on_final(&state, 0x22U, "Elk Pass", "ZZ99", VERIFY_NOW + 1U);
    MESH_TEST_FAIL_IF(strcmp(state.remote_name, "Elk Pass") != 0,
                      "the replacement did not take the slot");
    MESH_TEST_FAIL_IF(state.remote_node != 0U,
                      "a replaced exchange kept the previous peer's node, so its steps - and a "
                      "yes - would be addressed to the wrong node");
    record_success(test_name);
}

/*
 * Stop on the waiting sheet is a success, and it drops the INITIATE that has not gone out.
 *
 * The exchange deliberately has no nonce at that point - the radio has not answered - so the
 * nonce check every other step goes through would have called a press that did exactly what it
 * said a failure, and toasted one. Worse, the queued INITIATE stayed queued: the radio would
 * then open on the wire the ceremony the user had just stopped.
 */
MESH_TEST_CASE(key_verification_stop_before_the_radio_answers, unit) {
    struct mesh_session session;
    unsigned sends = 0U;
    key_trust_seed(&session, &sends, false);

    MESH_TEST_FAIL_IF(mesh_session_verify_key_begin(&session, 0x2001U) <= 0,
                      "the ceremony would not start");
    MESH_TEST_FAIL_IF(session.verification.nonce != 0U,
                      "a fresh initiation should have no nonce until the radio answers");

    const int stopped = mesh_session_verify_key_settle(&session, false);
    MESH_TEST_FAIL_IF(stopped < 0, "Stop before the radio answered was reported as a failure");
    MESH_TEST_FAIL_IF(mesh_key_verification_active(mesh_session_verification(&session)),
                      "Stop left the exchange open");

    /* And nothing addressed to that node is left waiting to go out. */
    struct mesh_admin_request request;
    while (mesh_radio_settings_next_request(&session.settings, 1000U, &request)) {
        MESH_TEST_FAIL_IF(request.kind == MESH_ADMIN_KEY_VERIFICATION,
                          "a stopped ceremony still had a step queued to send");
        session.settings.pending_request_id = 0U;
    }
    record_success(test_name);
}

/*
 * Answering the security number restarts the deadline.
 *
 * The step queues without moving the stage - the radio has the next move - so the five minutes
 * would otherwise have gone on running from the moment the radio asked. An answer given at four
 * minutes fifty-nine would have been expired a second later, with a DO_NOT_VERIFY queued behind
 * the response that was about to succeed: a timely answer cancelling its own ceremony.
 */
MESH_TEST_CASE(key_verification_answering_the_number_restarts_the_clock, unit) {
    struct mesh_key_verification state;
    mesh_key_verification_reset(&state);
    (void)mesh_key_verification_on_number_request(&state, 0x33U, "Pine Ridge", VERIFY_NOW);

    const uint32_t nearly = VERIFY_NOW + MESH_KEY_VERIFICATION_TIMEOUT_SECONDS - 1U;
    MESH_TEST_FAIL_IF(!mesh_key_verification_touch(&state, nearly), "the deadline did not move");
    MESH_TEST_FAIL_IF(state.stage != (uint8_t)MESH_KEY_VERIFICATION_ENTER_NUMBER,
                      "restamping the deadline moved the stage");
    MESH_TEST_FAIL_IF(mesh_key_verification_tick(
                          &state, VERIFY_NOW + MESH_KEY_VERIFICATION_TIMEOUT_SECONDS, NULL),
                      "an exchange answered a second ago was expired on the radio's own clock");

    /* A clock of 0 restamps nothing, on the same terms as the tick: a client that cannot measure
       five minutes has no deadline to move. */
    MESH_TEST_FAIL_IF(mesh_key_verification_touch(&state, 0U),
                      "a client with no clock restamped a deadline anyway");
    record_success(test_name);
}

/*
 * A pairing prompt and a verification prompt are never both up.
 *
 * They are the only two overlays the *radio* raises rather than a press, and they collided:
 * both flavours are the same keyboard, so raising the verification one over an open pairing PIN
 * left both flags true. The dispatch and the renderer both prefer the passkey and closing it
 * cleared the pair, so the verification prompt was never drawn, never answerable and never
 * reopened. The sheet was the same bug one level up - it takes keys ahead of the keyboard, so it
 * would have made the PIN untypeable while the bond timed out underneath it.
 *
 * Pairing wins, because it is blocking a bond on a thirty-second clock against this one's five
 * minutes - and the store says whether the overlay is up so the app can try again rather than
 * recording a question it never asked.
 */
MESH_TEST_CASE(key_verification_defers_to_a_pairing_prompt, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    mesh_ui_store_open_passkey_prompt(&store, "Pine Ridge", 0U, false);
    MESH_TEST_FAIL_IF(!store.nav.keyboard_passkey, "the pairing prompt did not open");

    struct mesh_ui_verification verification;
    memset(&verification, 0, sizeof verification);
    verification.stage = (uint8_t)MESH_UI_VERIFY_ENTER_NUMBER;
    verification.remote_node = 0x2001U;
    mesh_ui_store_set_verification(&store, &verification);

    MESH_TEST_FAIL_IF(mesh_ui_store_open_verify_number(&store),
                      "the number prompt claimed the keyboard a pairing PIN was holding");
    MESH_TEST_FAIL_IF(store.nav.keyboard_verify,
                      "both keyboard flavours ended up set, which draws neither");
    MESH_TEST_FAIL_IF(!store.nav.keyboard_passkey, "the pairing prompt was displaced");

    MESH_TEST_FAIL_IF(mesh_ui_store_open_verify_sheet(&store),
                      "the sheet opened over a pairing prompt it would have made untypeable");
    MESH_TEST_FAIL_IF(store.nav.verify_open, "the sheet is up over the pairing prompt");

    /* Once the bond is answered the prompt comes up on the next try - which is what the app's
       "record the stage only when it is shown" rule buys. */
    mesh_ui_store_close_passkey_prompt(&store);
    MESH_TEST_FAIL_IF(!mesh_ui_store_open_verify_number(&store),
                      "the number prompt did not open once pairing was out of the way");
    MESH_TEST_FAIL_IF(!store.nav.keyboard_verify, "the keyboard is not in verification mode");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}
