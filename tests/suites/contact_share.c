#define _POSIX_C_SOURCE 200809L

/*
 * Contact sharing: the `meshtastic.org/v/#` link, and the two directions across a radio's
 * NodeDB.
 *
 * The suite beside tests/suites/channel_share.c and deliberately much shorter, because the
 * pieces underneath it are already held there: base64 is the same codec, the QR encoder is the
 * same encoder, and the wrapper rule both links follow is one function
 * (src/proto/link_url.h) with cases on both sides of it. What is only here is the message -
 * a `SharedContact` rather than a `ChannelSet` - and the one thing this direction has that the
 * other does not: two fields in an incoming link that are *instructions to the reader's radio*
 * rather than statements about the sender's, which this client refuses to carry out.
 *
 * That refusal is the case worth reading first. A contact link is a stranger's bytes arriving
 * with nothing to authenticate them, so a link that could set `manually_verified` would be a
 * way to put a padlock beside a key nobody ever checked - which is the whole of what the
 * padlock in this client is for.
 */

#include "framework/mesh_test.h"

#include "mesh/core/contact_share.h"
#include "mesh/core/session.h"
#include "mesh/proto/contact_url.h"
#include "mesh/ui/contact_share.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/utils/qr.h"
#include "support/session_fixture.h"
#include "support/ui_fixture.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- the link ------------------------------------------------------------------------------ */

static const uint8_t k_key[32] = {
    0x9AU, 0x1BU, 0x00U, 0xFFU, 0x3CU, 0x51U, 0x7DU, 0x02U, 0xE4U, 0x88U, 0x10U,
    0x6FU, 0xA3U, 0x44U, 0x29U, 0xC7U, 0x5DU, 0x11U, 0xBEU, 0x70U, 0x36U, 0x92U,
    0xCAU, 0x08U, 0xF1U, 0x4DU, 0x63U, 0xABU, 0x27U, 0x99U, 0x0EU, 0x55U,
};

static void fill_contact(meshtastic_SharedContact *contact, uint32_t node_num, const char *name) {
    *contact = (meshtastic_SharedContact)meshtastic_SharedContact_init_zero;
    contact->node_num = node_num;
    contact->has_user = true;
    snprintf(contact->user.id, sizeof contact->user.id, "!%08x", node_num);
    snprintf(contact->user.long_name, sizeof contact->user.long_name, "%s", name);
    snprintf(contact->user.short_name, sizeof contact->user.short_name, "%.4s", name);
    contact->user.hw_model = meshtastic_HardwareModel_TBEAM;
    contact->user.public_key.size = (pb_size_t)sizeof k_key;
    memcpy(contact->user.public_key.bytes, k_key, sizeof k_key);
}

MESH_TEST_CASE(contact_url_round_trips, unit) {
    meshtastic_SharedContact contact;
    fill_contact(&contact, 0xA1B2C3D4U, "Trail Boss");

    char url[MESH_CONTACT_URL_MAX];
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&contact, url, sizeof url) == 0U,
                      "a contact did not encode");
    MESH_TEST_FAIL_IF(strncmp(url, MESH_CONTACT_URL_PREFIX, strlen(MESH_CONTACT_URL_PREFIX)) != 0,
                      "the link does not start with the Meshtastic contact prefix");
    /* `/v/` and not `/e/`: the two payloads will not decode as each other, and a reader that
       guessed would hand a channel set to the NodeDB. */
    MESH_TEST_FAIL_IF(strstr(url, "/v/#") == NULL, "the link is not on the contact path");
    MESH_TEST_FAIL_IF(strpbrk(url + strlen(MESH_CONTACT_URL_PREFIX), "+/=") != NULL,
                      "the payload is not in the URL-safe alphabet");

    meshtastic_SharedContact back;
    MESH_TEST_FAIL_IF(!mesh_contact_url_decode(url, &back), "the link did not decode");
    MESH_TEST_FAIL_IF(back.node_num != 0xA1B2C3D4U, "the node number did not survive");
    MESH_TEST_FAIL_IF(!back.has_user || strcmp(back.user.long_name, "Trail Boss") != 0,
                      "the name did not survive");
    MESH_TEST_FAIL_IF(strcmp(back.user.id, "!a1b2c3d4") != 0, "the user id did not survive");
    MESH_TEST_FAIL_IF(back.user.public_key.size != sizeof k_key ||
                          memcmp(back.user.public_key.bytes, k_key, sizeof k_key) != 0,
                      "the public key did not survive");
    MESH_TEST_FAIL_IF(back.user.hw_model != meshtastic_HardwareModel_TBEAM,
                      "the hardware model did not survive");

    /* The bound in the header is the protobuf's own, so the longest contact there can be still
       fits the buffer every caller carries. */
    meshtastic_SharedContact wide;
    fill_contact(&wide, 0xFFFFFFFFU, "");
    memset(wide.user.long_name, 'W', sizeof wide.user.long_name - 1U);
    memset(wide.user.short_name, 'S', sizeof wide.user.short_name - 1U);
    /* The id stays canonical - the decoder now refuses one that disagrees with the node number,
       so the widest *legal* contact is this rather than sixteen characters of filler. */
    wide.should_ignore = true;
    wide.manually_verified = true;
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&wide, url, sizeof url) == 0U,
                      "the widest possible contact did not fit MESH_CONTACT_URL_MAX");
    record_success(test_name);
}

/*
 * What the decoder is forgiving about, and what it is not.
 *
 * The forgiveness is the channel link's, for its reason: the only way one of these reaches this
 * client is somebody reading it off a phone and typing it on an on-screen keyboard, so a bare
 * payload has to work. The strictness is this format's own - a contact with no key is not a
 * contact, because a key is the whole of what the link exists to carry ahead of the node.
 */
MESH_TEST_CASE(contact_url_decode_is_strict_about_the_payload, unit) {
    meshtastic_SharedContact contact;
    fill_contact(&contact, 0x0000BEEFU, "Ridge");

    char url[MESH_CONTACT_URL_MAX];
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&contact, url, sizeof url) == 0U,
                      "the contact did not encode");
    const char *payload = url + strlen(MESH_CONTACT_URL_PREFIX);

    char variant[MESH_CONTACT_URL_MAX + 64U];
    meshtastic_SharedContact back;

    MESH_TEST_FAIL_IF(!mesh_contact_url_decode(payload, &back), "a bare payload was refused");
    snprintf(variant, sizeof variant, "https://mesh.example.org/v/#%s", payload);
    MESH_TEST_FAIL_IF(!mesh_contact_url_decode(variant, &back),
                      "a link off another host was refused");
    /* A tail this format has no use for still comes off, so a link that arrived with somebody's
       tracking parameter on it is not refused over characters that were never the payload. */
    snprintf(variant, sizeof variant, "%s?utm_source=phone", url);
    MESH_TEST_FAIL_IF(!mesh_contact_url_decode(variant, &back), "a query tail was not ignored");

    /* And what it will not do is guess. */
    MESH_TEST_FAIL_IF(mesh_contact_url_decode("https://meshtastic.org/v/#not base64", &back),
                      "a payload off the alphabet decoded");
    MESH_TEST_FAIL_IF(mesh_contact_url_decode(MESH_CONTACT_URL_PREFIX, &back),
                      "an empty payload decoded");
    MESH_TEST_FAIL_IF(mesh_contact_url_decode("", &back), "an empty string decoded");
    MESH_TEST_FAIL_IF(mesh_contact_url_decode(NULL, &back), "NULL decoded");

    /*
     * The two halves that make a contact usable, refused at both ends.
     *
     * A payload of zero bytes decodes cleanly into an empty SharedContact - every field on the
     * wire is optional - so without this bar an empty fragment would be a valid contact naming
     * node 0 with no key, and the refusal would surface three layers down as a queue error with
     * nothing to say.
     */
    meshtastic_SharedContact keyless = contact;
    keyless.user.public_key.size = 0U;
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&keyless, url, sizeof url) != 0U,
                      "a contact with no key produced a link");
    MESH_TEST_FAIL_IF(url[0] != '\0', "a refused encode left text behind");

    meshtastic_SharedContact nameless = contact;
    nameless.node_num = 0U;
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&nameless, url, sizeof url) != 0U,
                      "a contact with no node number produced a link");
    record_success(test_name);
}

/*
 * A contact that names two different nodes is not a contact.
 *
 * `SharedContact` says who it is twice, and nothing on the wire makes the two agree: `node_num`
 * is what the radio files the entry under, `user.id` is what a screen would show. A link where
 * they differ reads as one node and is written as another - approve `!aaaaaaaa` and a
 * stranger's key lands in `!bbbbbbbb`'s NodeDB slot - so the decoder refuses it and every
 * caller above may treat the two as interchangeable.
 */
MESH_TEST_CASE(contact_url_refuses_a_contradictory_id, unit) {
    meshtastic_SharedContact contact;
    fill_contact(&contact, 0xAAAAAAAAU, "Impostor");
    char url[MESH_CONTACT_URL_MAX];
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&contact, url, sizeof url) == 0U,
                      "a contact whose halves agree did not encode");

    /* The id naming a different node than the number: refused at both ends. */
    meshtastic_SharedContact lying = contact;
    snprintf(lying.user.id, sizeof lying.user.id, "!bbbbbbbb");
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&lying, url, sizeof url) != 0U,
                      "a contact naming two nodes produced a link");
    /*
     * And on the way in, which is the half that matters: this is the payload an attacker writes
     * with their own encoder, so it is built by hand rather than round-tripped through ours.
     *
     * The control beside it is the same bytes with the id corrected, and it is here to prove
     * what the refusal is *for*. Without it a typo in the literal would fail to decode for
     * being malformed and the case would pass having tested nothing.
     */
    static const char k_lying[] = "https://meshtastic.org/v/#"
                                  "CKrVqtUKEj0KCSFiYmJiYmJiYhIISW1wb3N0b3IaBEltcG9CIBAREhMUFRYXGBka"
                                  "GxwdHh8gISIjJCUmJygpKissLS4v";
    static const char k_control[] = "https://meshtastic.org/v/#"
                                    "CKrVqtUKEj0KCSFhYWFhYWFhYRIISW1wb3N0b3IaBEltcG9CIBAREhMUFRYXGB"
                                    "kaGxwdHh8gISIjJCUmJygpKissLS4v";
    meshtastic_SharedContact back;
    MESH_TEST_FAIL_IF(mesh_contact_url_decode(k_lying, &back),
                      "a contact naming two nodes decoded");
    MESH_TEST_FAIL_IF(!mesh_contact_url_decode(k_control, &back),
                      "the control payload did not decode, so the case above proved nothing");
    MESH_TEST_FAIL_IF(back.node_num != 0xAAAAAAAAU || strcmp(back.user.id, "!aaaaaaaa") != 0 ||
                          back.user.public_key.size != 32U,
                      "the control payload is not the contact the lying one claims to be");

    /* An empty id claims nothing, so it is allowed - the node number is the field that counts. */
    meshtastic_SharedContact silent = contact;
    silent.user.id[0] = '\0';
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&silent, url, sizeof url) == 0U,
                      "a contact that leaves its id out was refused");
    MESH_TEST_FAIL_IF(!mesh_contact_url_decode(url, &back) || back.node_num != 0xAAAAAAAAU,
                      "a contact with no id did not decode");

    /* And upstream's spelling in capitals is read rather than refused. */
    meshtastic_SharedContact shouty = contact;
    snprintf(shouty.user.id, sizeof shouty.user.id, "!AAAAAAAA");
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&shouty, url, sizeof url) == 0U,
                      "an id in capitals was refused");

    /* The screens name the node the write will land on, which is now the only reading there
       is. */
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&contact, url, sizeof url) == 0U,
                      "the contact did not encode");
    char headline[96];
    char body[256];
    MESH_TEST_FAIL_IF(
        !mesh_ui_contact_import_sheet(url, headline, sizeof headline, body, sizeof body) ||
            strstr(body, "!aaaaaaaa") == NULL,
        "the sheet did not name the node the write is filed under");
    record_success(test_name);
}

/* The other half of the channel suite's capacity case: the longest link a SharedContact can
   make still fits the largest code this client draws, so a contact can always be shown as a
   picture rather than only as text. */
MESH_TEST_CASE(contact_url_always_fits_a_code, unit) {
    static uint8_t longest[MESH_CONTACT_URL_MAX - 1U];
    memset(longest, 'A', sizeof longest);
    struct mesh_qr qr;
    MESH_TEST_FAIL_IF(!mesh_qr_encode(longest, sizeof longest, MESH_QR_ECC_LOW, &qr),
                      "the longest possible contact link did not fit in a code");
    record_success(test_name);
}

/* ---- the radio's side ---------------------------------------------------------------------- */

/* A radio that has answered for its owner and its security config, which is the whole of what
   sharing a contact waits on - one reply rather than the channel table's nine. */
static void seed_radio(struct mesh_radio_settings *settings) {
    memset(settings, 0, sizeof *settings);
    settings->has_owner = true;
    settings->owner = (meshtastic_User)meshtastic_User_init_zero;
    snprintf(settings->owner.id, sizeof settings->owner.id, "!%08x", 0x12345678U);
    snprintf(settings->owner.long_name, sizeof settings->owner.long_name, "Brick One");
    snprintf(settings->owner.short_name, sizeof settings->owner.short_name, "BRK1");
    settings->owner.hw_model = meshtastic_HardwareModel_TBEAM;
    settings->has_security = true;
    settings->security.public_key.size = (pb_size_t)sizeof k_key;
    memcpy(settings->security.public_key.bytes, k_key, sizeof k_key);
}

MESH_TEST_CASE(contact_share_builds_this_radios_own_record, unit) {
    struct mesh_radio_settings settings;
    seed_radio(&settings);

    MESH_TEST_FAIL_IF(!mesh_contact_share_settled(&settings),
                      "a radio that has answered for its owner and key is not settled");

    meshtastic_SharedContact contact;
    MESH_TEST_FAIL_IF(!mesh_contact_share_build(&settings, &contact), "the contact did not build");
    /* The node number comes back out of the owner's own id, so the two halves of the contact
       cannot be assembled from readings taken a second apart. */
    MESH_TEST_FAIL_IF(contact.node_num != 0x12345678U, "the node number did not come from the id");
    MESH_TEST_FAIL_IF(strcmp(contact.user.long_name, "Brick One") != 0, "the owner did not carry");
    MESH_TEST_FAIL_IF(contact.user.public_key.size != sizeof k_key ||
                          memcmp(contact.user.public_key.bytes, k_key, sizeof k_key) != 0,
                      "the public key did not carry");
    /* Never ours to send: see mesh/core/contact_share.h. */
    MESH_TEST_FAIL_IF(contact.manually_verified,
                      "a shared contact claimed the reader had verified it");
    MESH_TEST_FAIL_IF(contact.should_ignore, "a shared contact asked the reader to ignore it");

    /* The owner's own copy of the key is preferred, because that is the one the mesh hears. */
    struct mesh_radio_settings both = settings;
    both.owner.public_key.size = 4U;
    memset(both.owner.public_key.bytes, 0x5AU, 4U);
    MESH_TEST_FAIL_IF(!mesh_contact_share_build(&both, &contact), "the contact did not build");
    MESH_TEST_FAIL_IF(contact.user.public_key.size != 4U ||
                          contact.user.public_key.bytes[0] != 0x5AU,
                      "the owner's own key was not preferred over SecurityConfig's");

    /* And the whole thing waits: no owner, no key, or an id that is not a node number. */
    struct mesh_radio_settings pending = settings;
    pending.has_owner = false;
    MESH_TEST_FAIL_IF(mesh_contact_share_settled(&pending) ||
                          mesh_contact_share_build(&pending, &contact),
                      "a radio that has not sent its owner was shared anyway");

    struct mesh_radio_settings no_key = settings;
    no_key.has_security = false;
    MESH_TEST_FAIL_IF(mesh_contact_share_settled(&no_key) ||
                          mesh_contact_share_build(&no_key, &contact),
                      "a radio with no public key was shared anyway");

    struct mesh_radio_settings bad_id = settings;
    snprintf(bad_id.owner.id, sizeof bad_id.owner.id, "nonsense");
    MESH_TEST_FAIL_IF(mesh_contact_share_build(&bad_id, &contact),
                      "an owner id that is not a node number was guessed at");

    /* The URL is the same thing through the encoder, and reads back as what went in. */
    char url[MESH_CONTACT_URL_MAX];
    MESH_TEST_FAIL_IF(mesh_contact_share_url(&settings, url, sizeof url) == 0U,
                      "the radio's own contact did not make a link");
    meshtastic_SharedContact back;
    MESH_TEST_FAIL_IF(!mesh_contact_url_decode(url, &back) || back.node_num != 0x12345678U,
                      "this radio's own link did not decode back to this radio");
    MESH_TEST_FAIL_IF(mesh_contact_share_url(&pending, url, sizeof url) != 0U || url[0] != '\0',
                      "a radio that has not answered produced a link");
    record_success(test_name);
}

/*
 * The case this suite exists for: a link does not get to set the two fields that are
 * instructions to *this* radio.
 *
 * `manually_verified` is the sharper of the two. It sets the bit that means "a person checked
 * this key out of band", which is what the padlock in this client reports - so a link that
 * could set it would launder trust through a picture on a screen, and the ceremony in
 * src/core/key_verification.c would be reporting something nobody did.
 */
MESH_TEST_CASE(contact_import_drops_the_senders_claims, unit) {
    struct mesh_radio_settings settings;
    seed_radio(&settings);

    meshtastic_SharedContact theirs;
    fill_contact(&theirs, 0x0BADF00DU, "Stranger");
    theirs.should_ignore = true;
    theirs.manually_verified = true;

    const int queued = mesh_contact_share_queue_import(&settings, &theirs);
    MESH_TEST_FAIL_IF(queued <= 0, "the contact was not queued");

    /* The passkey refresh comes first and the contact behind it, which is the shape every write
       in this client takes; the second entry is the one carrying the payload. */
    const struct mesh_admin_request *written = NULL;
    for (size_t i = 0; i < settings.queue_len; ++i) {
        const struct mesh_admin_request *entry =
            &settings.queue[(settings.queue_head + i) % MESH_RADIO_SETTINGS_FETCH_MAX];
        if (entry->kind == MESH_ADMIN_ADD_CONTACT) {
            written = entry;
        }
    }
    MESH_TEST_FAIL_IF(written == NULL, "no add_contact request was queued");
    MESH_TEST_FAIL_IF(written->payload.contact.node_num != 0x0BADF00DU,
                      "the queued contact is not the one in the link");
    MESH_TEST_FAIL_IF(written->payload.contact.user.public_key.size != sizeof k_key,
                      "the key did not reach the queue");
    MESH_TEST_FAIL_IF(written->payload.contact.manually_verified,
                      "a link talked this radio into marking a key verified");
    MESH_TEST_FAIL_IF(written->payload.contact.should_ignore,
                      "a link talked this radio into ignoring a node");

    /* The caller's own copy is left as the link wrote it: what a link said is worth keeping
       intact for anything that wants to describe it. */
    MESH_TEST_FAIL_IF(!theirs.manually_verified || !theirs.should_ignore,
                      "the import edited the caller's contact");

    /* And the bar underneath, which is the admin verb's own. */
    struct mesh_radio_settings fresh;
    seed_radio(&fresh);
    meshtastic_SharedContact keyless = theirs;
    keyless.user.public_key.size = 0U;
    MESH_TEST_FAIL_IF(mesh_contact_share_queue_import(&fresh, &keyless) >= 0,
                      "a contact with no key was queued");
    MESH_TEST_FAIL_IF(mesh_contact_share_queue_import(&fresh, NULL) >= 0, "NULL was queued");
    record_success(test_name);
}

/*
 * An imported contact has to reach *this client's* roster, not only the radio's NodeDB.
 *
 * The case the feature is named for. `add_contact` puts the node in the radio's database, but
 * the Nodes tab, the conversation list and every message destination here are built from
 * `handshake.nodes` - so without the seeding half, a contact for a node that has never
 * transmitted would be written to the radio and then be invisible in this client until the node
 * transmitted anyway, which is precisely the wait the link exists to skip.
 */
static int contact_sink(void *ctx, const uint8_t *packet, size_t len, uint32_t packet_id) {
    (void)packet;
    (void)len;
    (void)packet_id;
    if (ctx != NULL) {
        *(unsigned *)ctx += 1U;
    }
    return 0;
}

/* The roster row for a node, through the public accessor: mesh_session_find_node() is static to
   src/core/session.c, and the handshake status is how everything outside it reads the list -
   including the two screens this case is really about. */
static const struct mesh_node_summary *roster_row(const struct mesh_session *session,
                                                  uint32_t node_id) {
    const struct mesh_handshake_status *status = mesh_session_handshake(session);
    if (status == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (status->nodes[i].node_id == node_id) {
            return &status->nodes[i];
        }
    }
    return NULL;
}

MESH_TEST_CASE(contact_import_reaches_this_clients_roster, unit) {
    struct mesh_session session;
    unsigned sends = 0U;
    mesh_session_init(&session);
    mesh_session_attach(&session, contact_sink, &sends);

    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = 0x1000U;
    (void)mesh_test_session_feed_from_radio(&session, &my_info);

    const char *failure = NULL;
    meshtastic_SharedContact theirs;
    fill_contact(&theirs, 0x0BADF00DU, "Stranger");

    if (roster_row(&session, 0x0BADF00DU) != NULL) {
        failure = "a node nobody has heard of was already in the roster";
        goto cleanup;
    }
    if (mesh_session_import_contact(&session, &theirs) <= 0) {
        failure = "the import queued nothing";
        goto cleanup;
    }

    const struct mesh_node_summary *added = roster_row(&session, 0x0BADF00DU);
    if (added == NULL) {
        failure = "the imported contact never reached this client's roster";
        goto cleanup;
    }
    if (strcmp(added->long_name, "Stranger") != 0 || added->public_key_len != 32U ||
        memcmp(added->public_key, k_key, sizeof k_key) != 0) {
        failure = "the roster record did not carry the link's name and key";
        goto cleanup;
    }
    if (strcmp(added->user_id, "!0badf00d") != 0) {
        failure = "the roster record is not filed under the node the write names";
        goto cleanup;
    }
    /* The two the radio is the authority on, and neither is ours to claim. A link is not a
       ceremony, and the ack for an AdminMessage is not the ack for an insertion - what settles
       `in_nodedb` is the node's next NodeInfo. */
    if (added->key_verified) {
        failure = "a link put a verified mark on a key nobody checked";
        goto cleanup;
    }
    if (added->in_nodedb) {
        failure = "the roster claimed the radio had stored the contact before it said so";
        goto cleanup;
    }
    if (added->last_heard != 0U) {
        failure = "a node that has never transmitted was given a last-heard time";
        goto cleanup;
    }

    /*
     * And a second import may not overwrite a node we have actually heard. The roster record is
     * first-hand; a link is a third party's account of the same node.
     */
    meshtastic_FromRadio heard = meshtastic_FromRadio_init_default;
    heard.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    heard.node_info.num = 0x2002U;
    heard.node_info.has_user = true;
    snprintf(heard.node_info.user.id, sizeof heard.node_info.user.id, "!00002002");
    snprintf(heard.node_info.user.long_name, sizeof heard.node_info.user.long_name, "Real Name");
    heard.node_info.user.public_key.size = 32U;
    heard.node_info.user.public_key.bytes[0] = 0xA1U;
    (void)mesh_test_session_feed_from_radio(&session, &heard);

    meshtastic_SharedContact rename;
    fill_contact(&rename, 0x2002U, "Not Their Name");
    (void)mesh_session_import_contact(&session, &rename);
    const struct mesh_node_summary *known = roster_row(&session, 0x2002U);
    if (known == NULL || strcmp(known->long_name, "Real Name") != 0 ||
        known->public_key[0] != 0xA1U) {
        failure = "a link overwrote the record of a node this radio had heard for itself";
        goto cleanup;
    }

cleanup:
    mesh_session_detach(&session);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* ---- the two screens ------------------------------------------------------------------------ */

#define NO_ROW UINT32_MAX

/* Which row of the open User section carries `which`, or NO_ROW. Asked of the item list rather
   than counted, so a section that grows a field above these two does not move the test. */
static uint32_t user_action_row(const struct mesh_ui_store *store,
                                enum mesh_ui_settings_action which) {
    struct mesh_ui_settings_item items[32];
    /* The roster the way every public caller reaches it. `mesh_ui_nav_handshake()` is the same
       question in one call, but it lives in src/ui/nav_internal.h - which is the group's own
       header and not something outside it may include (CLAUDE.md). */
    const uint32_t count =
        mesh_ui_settings_items(&store->settings, store->handshake_valid ? &store->handshake : NULL,
                               NULL, 0U, MESH_UI_SETTINGS_USER, MESH_UI_SETTINGS_NO_CHANNEL, items,
                               (uint32_t)(sizeof items / sizeof items[0]));
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].kind == MESH_UI_SETTING_ACTION && items[i].number == (uint32_t)which) {
            return i;
        }
    }
    return NO_ROW;
}

MESH_TEST_CASE(contact_share_rows_drive_the_two_screens, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_owner = true;
    /* The radio is answering admin, which is the whole of what the add row waits on: unlike a
       channel import this overwrites no table, so there is no settled reading of one to need. */
    settings.admin_ok = true;
    snprintf(settings.long_name, sizeof settings.long_name, "Brick One");
    snprintf(settings.short_name, sizeof settings.short_name, "BRK1");

    struct mesh_radio_settings radio;
    seed_radio(&radio);
    (void)mesh_contact_share_url(&radio, settings.contact_url, sizeof settings.contact_url);
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    mesh_test_settings_open(&store, MESH_UI_SETTINGS_USER);

    /*
     * The two rows are found by the verb they carry rather than by counting down from the top
     * of the section: the User list is fields first and these two last, and a fifth field
     * arriving above them would silently move a fixed index onto the wrong row.
     */
    const uint32_t show_row = user_action_row(&store, MESH_UI_SETTINGS_ACTION_SHARE_CONTACT);
    const uint32_t add_row = user_action_row(&store, MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT);
    if (show_row == NO_ROW || add_row == NO_ROW) {
        failure = "the User section should offer both contact rows";
        goto cleanup;
    }

    /* The show row goes when there is nothing to show, which is the clamp the screen needs: a
       radio swap leaves a level whose whole content has gone. The add row stays, because what
       it needs is a radio answering admin rather than anything to show. */
    {
        struct mesh_ui_settings pending = settings;
        pending.contact_url[0] = '\0';
        mesh_ui_store_set_settings(&store, &pending);
        if (user_action_row(&store, MESH_UI_SETTINGS_ACTION_SHARE_CONTACT) != NO_ROW ||
            user_action_row(&store, MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT) == NO_ROW) {
            failure = "a radio with no contact link should offer to add one and not to show one";
            goto cleanup;
        }
        mesh_ui_store_set_settings(&store, &settings);
    }
    /* And neither before the radio has answered admin at all. */
    {
        struct mesh_ui_settings cold = settings;
        cold.admin_ok = false;
        cold.contact_url[0] = '\0';
        mesh_ui_store_set_settings(&store, &cold);
        if (user_action_row(&store, MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT) != NO_ROW) {
            failure = "a radio that is not answering admin should not offer to add a contact";
            goto cleanup;
        }
        mesh_ui_store_set_settings(&store, &settings);
    }

    /* ---- show: a row that opens a picture and nothing else ---- */
    if (!mesh_test_settings_cursor_to(&store, show_row)) {
        failure = "the show row could not be reached";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.contact_open || action.type != MESH_UI_ACTION_NONE) {
        failure = "A on the show row should open the sheet and ask the app for nothing";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.contact_open) {
        failure = "the contact sheet should ignore every key but B";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.contact_open) {
        failure = "B should close the contact sheet";
        goto cleanup;
    }

    /* What the screen puts under the code, read off the same characters it draws - which is
       what makes the caption a check on the picture rather than a second opinion about it. */
    char summary[160];
    if (!mesh_ui_contact_share_summary(settings.contact_url, summary, sizeof summary) ||
        strstr(summary, "Brick One") == NULL || strstr(summary, "!12345678") == NULL) {
        failure = "the contact summary should name the radio the code is for";
        goto cleanup;
    }
    if (mesh_ui_contact_share_summary("", summary, sizeof summary) || summary[0] != '\0') {
        failure = "an empty link should have no summary";
        goto cleanup;
    }

    /* ---- add: a row, a keyboard, a sheet ---- */
    if (!mesh_test_settings_cursor_to(&store, add_row)) {
        failure = "the add row could not be reached";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || !store.nav.keyboard_contact_url) {
        failure = "A on the add row should open the contact link keyboard";
        goto cleanup;
    }

    /* Done on something that is not a link leaves the user standing on the keyboard with what
       they typed: two hundred characters of base64 are not worth one wrong one. */
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "not a link");
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (!store.nav.keyboard_open || store.nav.confirm_open || store.nav.toast[0] == '\0') {
        failure = "a link that does not parse should stay on the keyboard and say so";
        goto cleanup;
    }
    /* A channel link is not a contact link, and the two must not be read as each other. */
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s",
             "https://meshtastic.org/e/#CgkSAQEaBFRlc3Q");
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (!store.nav.keyboard_open || store.nav.confirm_open) {
        failure = "a channel link should not be taken as a contact";
        goto cleanup;
    }

    meshtastic_SharedContact theirs;
    fill_contact(&theirs, 0x0BADF00DU, "Stranger");
    char link[MESH_CONTACT_URL_MAX];
    if (mesh_contact_url_encode(&theirs, link, sizeof link) == 0U) {
        failure = "the link did not encode";
        goto cleanup;
    }
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", link);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (store.nav.keyboard_open || !store.nav.confirm_open ||
        store.nav.confirm_action != (uint8_t)MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT ||
        store.nav.confirm_cursor != 1U) {
        failure = "a link should close the keyboard and raise the sheet, on Cancel";
        goto cleanup;
    }

    /* The sheet names the node in its headline and its number in the paragraph: the name is
       what somebody was told to look for, and the number is what decides where the write
       lands. */
    char headline[96];
    char body[256];
    if (!mesh_ui_contact_import_sheet(store.nav.contact_url, headline, sizeof headline, body,
                                      sizeof body) ||
        strstr(headline, "Stranger") == NULL || strstr(body, "!0badf00d") == NULL) {
        failure = "the sheet should name the node being added and its number";
        goto cleanup;
    }

    /* And the answer is the only thing that reaches the app, carrying the link itself so the
       app can parse it against the session as it stands then. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action); /* onto the accept button */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_IMPORT_CONTACT ||
        strcmp(action.text, link) != 0) {
        failure = "the sheet's accept should hand the app the link";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A link carrying no name at all is still worth adding - the key is what it exists for - so the
   two places that have to put a node in front of a person fall back rather than drawing a gap,
   and they fall back the same way because they share the code that does it. */
MESH_TEST_CASE(contact_link_names_a_node_with_no_name, unit) {
    meshtastic_SharedContact bare;
    fill_contact(&bare, 0x00C0FFEEU, "");
    bare.user.long_name[0] = '\0';
    bare.user.short_name[0] = '\0';

    char link[MESH_CONTACT_URL_MAX];
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&bare, link, sizeof link) == 0U,
                      "a nameless contact did not encode");
    MESH_TEST_FAIL_IF(!mesh_ui_contact_link_valid(link), "a nameless contact is not a link");

    char name[64];
    MESH_TEST_FAIL_IF(!mesh_ui_contact_link_name(link, name, sizeof name) || name[0] == '\0',
                      "a nameless contact was left without a stand-in");

    char headline[96];
    MESH_TEST_FAIL_IF(!mesh_ui_contact_import_sheet(link, headline, sizeof headline, NULL, 0U),
                      "the sheet refused a nameless contact");
    MESH_TEST_FAIL_IF(strstr(headline, name) == NULL,
                      "the sheet and the toast name the same node differently");

    /* The short name is the second choice, ahead of the stand-in. */
    meshtastic_SharedContact short_only;
    fill_contact(&short_only, 0x00C0FFEEU, "");
    short_only.user.long_name[0] = '\0';
    snprintf(short_only.user.short_name, sizeof short_only.user.short_name, "RDG");
    MESH_TEST_FAIL_IF(mesh_contact_url_encode(&short_only, link, sizeof link) == 0U,
                      "the contact did not encode");
    MESH_TEST_FAIL_IF(!mesh_ui_contact_link_name(link, name, sizeof name) ||
                          strcmp(name, "RDG") != 0,
                      "the short name was not used when the long one was empty");

    MESH_TEST_FAIL_IF(mesh_ui_contact_link_valid(NULL), "NULL is a link");
    MESH_TEST_FAIL_IF(mesh_ui_contact_link_name("not a link", name, sizeof name),
                      "a name came out of something that is not a link");
    record_success(test_name);
}
