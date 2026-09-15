/*
 * The contact link reader, fed whatever arrives.
 *
 * Here for fuzz_channel_url.c's reason and with one sharper edge. What it reads is a string
 * somebody else wrote and a *person* carried across - read off a phone, typed on an on-screen
 * keyboard, or copied out of a forum post - and what it produces is a node number and a public
 * key this client will offer to write into the radio's NodeDB. A link that decoded to something
 * other than what its author meant is a direct message encrypted to the wrong person's key.
 *
 * Two parsers stacked again: base64 in the forgiving form, which walks a string of unknown
 * length and decides how many bytes came out of it, and then a nanopb decode of the result -
 * where the length prefixes inside the `User` are chosen by whoever wrote the link.
 *
 * Build with scripts/fuzz.sh; see docs/testing.md.
 */

#include "mesh/proto/contact_url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fuzz_broke(const char *what) {
    fprintf(stderr, "contact url contract broken: %s\n", what);
    fflush(stderr);
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The reader takes a C string, so the input is terminated here rather than the corpus having
       to learn to end in a zero byte. An embedded NUL simply cuts the link short, which is what
       it would do arriving through the keyboard. */
    if (size + 1U > MESH_CONTACT_URL_MAX) {
        return 0;
    }
    char text[MESH_CONTACT_URL_MAX];
    memcpy(text, data, size);
    text[size] = '\0';

    meshtastic_SharedContact contact;
    if (!mesh_contact_url_decode(text, &contact)) {
        return 0;
    }

    /*
     * What a caller is entitled to assume of a link that decoded. The first two are the bar the
     * decoder promises and the admin verb depends on - mesh_radio_settings_queue_contact()
     * refuses both, so a link that got past here without them would be a press that reports a
     * queue error instead of a contact.
     */
    if (contact.node_num == 0U) {
        fuzz_broke("a contact naming no node decoded");
    }
    if (!contact.has_user || contact.user.public_key.size == 0U) {
        fuzz_broke("a contact with no key decoded, which every caller treats as impossible");
    }
    if (contact.user.public_key.size > sizeof contact.user.public_key.bytes) {
        fuzz_broke("a key longer than the field that holds it");
    }
    /* The three strings a screen will put in front of a person, each of which something
       downstream hands to a formatter that walks to a NUL. */
    if (memchr(contact.user.id, '\0', sizeof contact.user.id) == NULL) {
        fuzz_broke("an unterminated user id");
    }
    if (memchr(contact.user.long_name, '\0', sizeof contact.user.long_name) == NULL) {
        fuzz_broke("an unterminated long name");
    }
    if (memchr(contact.user.short_name, '\0', sizeof contact.user.short_name) == NULL) {
        fuzz_broke("an unterminated short name");
    }

    /*
     * And the round trip, which is the claim the sharing half rests on: a contact that came out
     * of a link goes back into one. Not that the two strings match - an encoder writes one
     * spelling and a link may have arrived in another - but that the second reads back as the
     * same contact.
     */
    char again[MESH_CONTACT_URL_MAX];
    if (mesh_contact_url_encode(&contact, again, sizeof again) == 0U) {
        fuzz_broke("a contact that decoded would not encode again");
    }
    meshtastic_SharedContact back;
    if (!mesh_contact_url_decode(again, &back)) {
        fuzz_broke("a link this encoder wrote would not decode");
    }
    if (back.node_num != contact.node_num ||
        back.user.public_key.size != contact.user.public_key.size ||
        memcmp(back.user.public_key.bytes, contact.user.public_key.bytes,
               contact.user.public_key.size) != 0 ||
        strcmp(back.user.id, contact.user.id) != 0 ||
        strcmp(back.user.long_name, contact.user.long_name) != 0) {
        fuzz_broke("the round trip changed the node, its key or its name");
    }
    return 0;
}
