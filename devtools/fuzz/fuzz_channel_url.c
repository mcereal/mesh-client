/*
 * The channel link reader, fed whatever arrives.
 *
 * It belongs here for the reason the zip reader does, even though nothing it parses comes off
 * the air: what it reads is a string somebody else wrote and a *person* carried across - read
 * off a phone, typed on an on-screen keyboard, or copied out of a forum post - and what it
 * produces is a set of channels this client will offer to write to a radio. A link that decoded
 * to something other than what its author meant is a radio quietly moved onto a mesh.
 *
 * It is also two parsers stacked. Base64 in the forgiving form, which walks a string of unknown
 * length and decides how many bytes came out of it, and then a nanopb decode of the result -
 * where the length prefixes inside a `ChannelSet` are chosen by whoever wrote the link.
 *
 * Build with scripts/fuzz.sh; see docs/testing.md.
 */

#include "mesh/proto/channel_url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fuzz_broke(const char *what) {
    fprintf(stderr, "channel url contract broken: %s\n", what);
    fflush(stderr);
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The reader takes a C string, so the input is terminated here rather than the corpus having
       to learn to end in a zero byte. An embedded NUL simply cuts the link short, which is what
       it would do arriving through the keyboard. */
    if (size + 1U > MESH_CHANNEL_URL_MAX) {
        return 0;
    }
    char text[MESH_CHANNEL_URL_MAX];
    memcpy(text, data, size);
    text[size] = '\0';

    meshtastic_ChannelSet set;
    bool add = false;
    if (!mesh_channel_url_decode(text, &set, &add)) {
        return 0;
    }

    /*
     * What a caller is entitled to assume of a link that decoded, and therefore what an import
     * is built on. Every one of these is a bound something downstream indexes by.
     */
    if (set.settings_count == 0U) {
        fuzz_broke("an empty set decoded, which every caller treats as impossible");
    }
    if (set.settings_count > (pb_size_t)(sizeof set.settings / sizeof set.settings[0])) {
        fuzz_broke("more channels than the set can hold");
    }
    for (pb_size_t i = 0; i < set.settings_count; ++i) {
        const meshtastic_ChannelSettings *channel = &set.settings[i];
        if (channel->psk.size > sizeof channel->psk.bytes) {
            fuzz_broke("a key longer than the field that holds it");
        }
        if (memchr(channel->name, '\0', sizeof channel->name) == NULL) {
            fuzz_broke("an unterminated channel name");
        }
    }

    /*
     * And the round trip, which is the claim the share half of this feature rests on: a set that
     * came out of a link goes back into one. Not that the two strings match - an encoder writes
     * one spelling and a link may have arrived in another - but that the second one reads back
     * as the same set.
     */
    char again[MESH_CHANNEL_URL_MAX];
    if (mesh_channel_url_encode(&set, add, again, sizeof again) == 0U) {
        fuzz_broke("a set that decoded would not encode again");
    }
    meshtastic_ChannelSet back;
    bool back_add = false;
    if (!mesh_channel_url_decode(again, &back, &back_add)) {
        fuzz_broke("a link this encoder wrote would not decode");
    }
    if (back.settings_count != set.settings_count || back_add != add) {
        fuzz_broke("the round trip lost a channel or the add flag");
    }
    for (pb_size_t i = 0; i < set.settings_count; ++i) {
        if (back.settings[i].psk.size != set.settings[i].psk.size ||
            memcmp(back.settings[i].psk.bytes, set.settings[i].psk.bytes,
                   set.settings[i].psk.size) != 0 ||
            strcmp(back.settings[i].name, set.settings[i].name) != 0) {
            fuzz_broke("the round trip changed a channel's name or key");
        }
    }
    return 0;
}
