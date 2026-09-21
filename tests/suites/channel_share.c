#define _POSIX_C_SOURCE 200809L

/*
 * Channel sharing: the link, the code and the two directions across a radio's channel table.
 *
 * Four subjects, in one suite because they are one feature and each is only interesting in
 * terms of the next: base64 is here because a channel URL is base64 and a key is base64 with a
 * different alphabet and a stricter reading; the QR encoder is here because the only thing this
 * client ever encodes is that URL.
 *
 * The QR cases are the ones worth explaining. A QR code is not a format anything else in this
 * repository can check - there is no decoder here and there will not be one, because the Brick
 * has no camera to need it - so the matrices these cases pin were verified the only way that
 * proves anything: rendered to a bitmap and read back by a real reader (zxing-cpp), every
 * version from 1 to 25 at all four correction levels, plus the exact payloads below. What the
 * hash holds is that the encoder still produces *that* matrix. The structural cases beside it
 * are what says which part broke when it does not.
 */

#include "inkcell/utils/qr.h"
#include "inkwell/base/text.h"

#include "framework/mesh_test.h"

#include "inkwell/codec/base64.h"
#include "inkwell/codec/sha256.h"
#include "mesh/core/channel_share.h"
#include "mesh/proto/channel_url.h"
#include "mesh/ui/channel_share.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "support/ui_fixture.h"

#include <stdio.h>
#include <string.h>

/* ---- base64 ------------------------------------------------------------------------------ */

/* The key field still reads and writes the padded standard alphabet after being moved onto the
   shared codec - which is the form every Meshtastic app shows a key in. */
MESH_TEST_CASE(base64_key_text_is_the_padded_alphabet, unit) {
    const uint8_t key[16] = {0xD4U, 0xF1U, 0xBBU, 0x3AU, 0x20U, 0x29U, 0x07U, 0x59U,
                             0xF0U, 0xBCU, 0xFFU, 0xABU, 0xCFU, 0x4EU, 0x69U, 0x01U};
    char text[64];
    mesh_ui_settings_key_text(key, sizeof key, text, sizeof text);
    MESH_TEST_FAIL_IF(strcmp(text, "1PG7OiApB1nwvP+rz05pAQ==") != 0,
                      "the default key did not come out in the apps' own spelling");
    record_success(test_name);
}

/* ---- the QR encoder ------------------------------------------------------------------------ */

static void qr_digest(const struct inkcell_qr *qr, char *out, size_t out_len) {
    struct inkwell_sha256 ctx;
    uint8_t digest[INKWELL_SHA256_DIGEST_LEN];
    inkwell_sha256_init(&ctx);
    inkwell_sha256_update(&ctx, &qr->size, sizeof qr->size);
    inkwell_sha256_update(&ctx, qr->modules, (size_t)qr->size * qr->size);
    inkwell_sha256_final(&ctx, digest);
    inkwell_sha256_hex(digest, out, out_len);
}

/* A finder pattern is a 7x7 ring the reader locks onto; three of them say which way up a code
   is. Checked here rather than only through the hash because "the corners are wrong" and "the
   payload is wrong" are different bugs and the hash cannot tell them apart. */
static bool qr_finder_at(const struct inkcell_qr *qr, int ox, int oy) {
    static const char *k_rows[7] = {
        "1111111", "1000001", "1011101", "1011101", "1011101", "1000001", "1111111",
    };
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 7; ++x) {
            if (inkcell_qr_dark(qr, ox + x, oy + y) != (k_rows[y][x] == '1')) {
                return false;
            }
        }
    }
    return true;
}

MESH_TEST_CASE(qr_structure_is_a_qr_code, unit) {
    static const char k_text[] = MESH_CHANNEL_URL_PREFIX "CgkSAQEqA0FRSQ";
    struct inkcell_qr qr;
    MESH_TEST_FAIL_IF(
        !inkcell_qr_encode((const uint8_t *)k_text, strlen(k_text), INKCELL_QR_ECC_LOW, &qr),
        "a channel-sized URL did not encode");

    /* 21 + 4*(version - 1), so a size outside that series is not a QR code at all. */
    MESH_TEST_FAIL_IF(qr.size < 21U || qr.size > INKCELL_QR_MAX_SIZE || (qr.size - 17U) % 4U != 0U,
                      "the matrix is not a legal size");

    const int size = (int)qr.size;
    MESH_TEST_FAIL_IF(!qr_finder_at(&qr, 0, 0), "the top-left finder pattern is wrong");
    MESH_TEST_FAIL_IF(!qr_finder_at(&qr, size - 7, 0), "the top-right finder pattern is wrong");
    MESH_TEST_FAIL_IF(!qr_finder_at(&qr, 0, size - 7), "the bottom-left finder pattern is wrong");

    /* The timing patterns run between the finders, alternating, starting dark. */
    for (int i = 8; i < size - 8; ++i) {
        const bool dark = i % 2 == 0;
        MESH_TEST_FAIL_IF(inkcell_qr_dark(&qr, i, 6) != dark,
                          "the horizontal timing pattern broke");
        MESH_TEST_FAIL_IF(inkcell_qr_dark(&qr, 6, i) != dark, "the vertical timing pattern broke");
    }

    /* The one module that is dark in every code ever made. */
    MESH_TEST_FAIL_IF(!inkcell_qr_dark(&qr, 8, size - 8), "the dark module is light");

    /* Nothing outside the matrix, so a drawing loop that runs into the quiet zone is safe. */
    MESH_TEST_FAIL_IF(inkcell_qr_dark(&qr, -1, 0) || inkcell_qr_dark(&qr, 0, size) ||
                          inkcell_qr_dark(NULL, 0, 0),
                      "a module outside the matrix read as dark");
    record_success(test_name);
}

/*
 * The matrices, pinned.
 *
 * Both were read back by zxing-cpp before being written down; see the note at the top of this
 * file. A change here is either a bug or a deliberate change to what this encoder emits, and
 * either way it is not something to find out about from a phone that will not scan.
 */
MESH_TEST_CASE(qr_matrices_are_pinned, unit) {
    static const char k_link[] = MESH_CHANNEL_URL_PREFIX "CgkSAQEqA0FRSQ";
    static const struct {
        const char *label;
        enum inkcell_qr_ecc ecc;
        uint8_t size;
        const char *digest;
    } k_cases[] = {
        /* Version 3 at either level: a one-channel link is what almost every share is. */
        {"a channel link at low correction", INKCELL_QR_ECC_LOW, 29U,
         "529e20d462f2ce3f8c3be54befe19654e4c00d24bd7db25a3de3e6527a228c47"},
        {"the same link at medium", INKCELL_QR_ECC_MEDIUM, 29U,
         "701265f731173ebc5c6a9f660ceefa3d2cad4de9130e457de14e221e5f42fb66"},
    };

    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; ++i) {
        struct inkcell_qr qr;
        char reason[160];
        if (!inkcell_qr_encode((const uint8_t *)k_link, strlen(k_link), k_cases[i].ecc, &qr)) {
            snprintf(reason, sizeof reason, "%s did not encode", k_cases[i].label);
            record_failure(test_name, reason);
            return;
        }
        if (qr.size != k_cases[i].size) {
            snprintf(reason, sizeof reason, "%s came out %u modules across, not %u",
                     k_cases[i].label, (unsigned)qr.size, (unsigned)k_cases[i].size);
            record_failure(test_name, reason);
            return;
        }
        char digest[INKWELL_SHA256_DIGEST_LEN * 2U + 1U];
        qr_digest(&qr, digest, sizeof digest);
        if (strcmp(digest, k_cases[i].digest) != 0) {
            snprintf(reason, sizeof reason, "%s is not the matrix that was read back: %s",
                     k_cases[i].label, digest);
            record_failure(test_name, reason);
            return;
        }
    }
    record_success(test_name);
}

/* A payload past what the largest version holds is refused, and the matrix is left empty rather
   than half drawn: a caller that draws a failed code draws nothing. */
MESH_TEST_CASE(qr_refuses_what_it_cannot_hold, unit) {
    static uint8_t k_huge[4096];
    memset(k_huge, 'A', sizeof k_huge);
    struct inkcell_qr qr;
    MESH_TEST_FAIL_IF(inkcell_qr_encode(k_huge, sizeof k_huge, INKCELL_QR_ECC_LOW, &qr),
                      "an oversized payload encoded anyway");
    MESH_TEST_FAIL_IF(qr.size != 0U, "a refused encode left a matrix behind");

    /* The bound that matters to this client: the longest link a ChannelSet can make still
       fits, so a share can always be shown as a code rather than only as text. */
    static uint8_t k_longest[MESH_CHANNEL_URL_MAX - 1U];
    memset(k_longest, 'A', sizeof k_longest);
    MESH_TEST_FAIL_IF(!inkcell_qr_encode(k_longest, sizeof k_longest, INKCELL_QR_ECC_LOW, &qr),
                      "the longest possible channel link did not fit in a code");

    MESH_TEST_FAIL_IF(inkcell_qr_encode(NULL, 4U, INKCELL_QR_ECC_LOW, &qr), "NULL data encoded");
    MESH_TEST_FAIL_IF(inkcell_qr_encode(k_longest, 1U, INKCELL_QR_ECC_LOW, NULL),
                      "NULL output encoded");
    record_success(test_name);
}

/* ---- the link ------------------------------------------------------------------------------ */

static void fill_channel(meshtastic_ChannelSettings *settings, const char *name, uint8_t key) {
    *settings = (meshtastic_ChannelSettings)meshtastic_ChannelSettings_init_zero;
    snprintf(settings->name, sizeof settings->name, "%s", name);
    settings->psk.size = 1U;
    settings->psk.bytes[0] = key;
}

MESH_TEST_CASE(channel_url_round_trips, unit) {
    meshtastic_ChannelSet set = meshtastic_ChannelSet_init_zero;
    fill_channel(&set.settings[0], "LongFast", 1U);
    fill_channel(&set.settings[1], "Trail", 2U);
    set.settings_count = 2U;
    set.has_lora_config = true;
    set.lora_config.use_preset = true;
    set.lora_config.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    set.lora_config.hop_limit = 3U;

    char url[MESH_CHANNEL_URL_MAX];
    MESH_TEST_FAIL_IF(mesh_channel_url_encode(&set, false, url, sizeof url) == 0U,
                      "a two-channel set did not encode");
    MESH_TEST_FAIL_IF(strncmp(url, MESH_CHANNEL_URL_PREFIX, strlen(MESH_CHANNEL_URL_PREFIX)) != 0,
                      "the link does not start with the Meshtastic prefix");
    /* Base64 is URL-safe here, so nothing in the payload needs escaping in a fragment. */
    MESH_TEST_FAIL_IF(strpbrk(url + strlen(MESH_CHANNEL_URL_PREFIX), "+/=") != NULL,
                      "the payload is not in the URL-safe alphabet");

    meshtastic_ChannelSet back = meshtastic_ChannelSet_init_zero;
    bool add = true;
    MESH_TEST_FAIL_IF(!mesh_channel_url_decode(url, &back, &add), "the link did not decode");
    MESH_TEST_FAIL_IF(add, "a link with no query said it was an add");
    MESH_TEST_FAIL_IF(back.settings_count != 2U, "the channel count did not survive");
    MESH_TEST_FAIL_IF(strcmp(back.settings[0].name, "LongFast") != 0 ||
                          strcmp(back.settings[1].name, "Trail") != 0,
                      "the channel names did not survive");
    MESH_TEST_FAIL_IF(back.settings[1].psk.size != 1U || back.settings[1].psk.bytes[0] != 2U,
                      "a key did not survive");
    MESH_TEST_FAIL_IF(!back.has_lora_config ||
                          back.lora_config.region !=
                              meshtastic_Config_LoRaConfig_RegionCode_EU_868 ||
                          back.lora_config.hop_limit != 3U,
                      "the LoRa config did not survive");
    record_success(test_name);
}

/*
 * What the decoder is forgiving about, and what it is not.
 *
 * The forgiveness is not politeness: the only way a link reaches this client is somebody
 * reading it off a phone and typing it on an on-screen keyboard, so a bare payload has to work,
 * and so does one that arrived through a site that re-hosts the page.
 */
MESH_TEST_CASE(channel_url_decode_is_lenient_about_the_wrapper, unit) {
    meshtastic_ChannelSet set = meshtastic_ChannelSet_init_zero;
    fill_channel(&set.settings[0], "LongFast", 1U);
    set.settings_count = 1U;

    char url[MESH_CHANNEL_URL_MAX];
    MESH_TEST_FAIL_IF(mesh_channel_url_encode(&set, false, url, sizeof url) == 0U,
                      "the set did not encode");
    const char *payload = url + strlen(MESH_CHANNEL_URL_PREFIX);

    /* A whole link plus the suffix appended below. MESH_CHANNEL_URL_MAX already carries room
       for `?add=true` on a link the encoder wrote; the extra is for the worst case a reader of
       this line - the compiler included - has to assume, where `url` fills its own buffer. */
    char variant[MESH_CHANNEL_URL_MAX + sizeof("?add=true")];
    meshtastic_ChannelSet back;
    bool add = false;

    MESH_TEST_FAIL_IF(!mesh_channel_url_decode(payload, &back, &add) || back.settings_count != 1U,
                      "a bare payload was refused");
    snprintf(variant, sizeof variant, "https://www.meshtastic.org/d/#%s", payload);
    MESH_TEST_FAIL_IF(!mesh_channel_url_decode(variant, &back, &add) || back.settings_count != 1U,
                      "the older /d/ link was refused");
    snprintf(variant, sizeof variant, "%s?add=true", url);
    MESH_TEST_FAIL_IF(!mesh_channel_url_decode(variant, &back, &add), "an add link was refused");
    MESH_TEST_FAIL_IF(!add, "the add flag was not read");

    /* And what it will not do is guess. */
    MESH_TEST_FAIL_IF(mesh_channel_url_decode("https://meshtastic.org/e/#not base64", &back, NULL),
                      "a payload off the alphabet decoded");
    MESH_TEST_FAIL_IF(mesh_channel_url_decode(MESH_CHANNEL_URL_PREFIX, &back, NULL),
                      "an empty payload decoded");
    MESH_TEST_FAIL_IF(mesh_channel_url_decode("", &back, NULL), "an empty string decoded");
    MESH_TEST_FAIL_IF(mesh_channel_url_decode(NULL, &back, NULL), "NULL decoded");

    /* An empty set encodes to nothing rather than to a link that means nothing. */
    meshtastic_ChannelSet empty = meshtastic_ChannelSet_init_zero;
    MESH_TEST_FAIL_IF(mesh_channel_url_encode(&empty, false, url, sizeof url) != 0U,
                      "a set with no channels produced a link");
    MESH_TEST_FAIL_IF(url[0] != '\0', "a refused encode left text behind");
    record_success(test_name);
}

/* ---- the radio's side ---------------------------------------------------------------------- */

static void set_slot(struct mesh_radio_settings *settings, size_t slot, const char *name,
                     uint8_t key, meshtastic_Channel_Role role) {
    settings->has_channel[slot] = true;
    settings->channels[slot] = (meshtastic_Channel)meshtastic_Channel_init_zero;
    settings->channels[slot].index = (int8_t)slot;
    settings->channels[slot].role = role;
    settings->channels[slot].has_settings = true;
    fill_channel(&settings->channels[slot].settings, name, key);
}

/*
 * A radio that has *finished* answering: a primary, one secondary, the six disabled slots after
 * them, and a LoRa config.
 *
 * All eight slots rather than the two that carry anything, because that is what the end of a
 * sync looks like - the client asks for every index and the firmware answers for every index,
 * disabled ones included. A fixture that set only the two would be a radio frozen half way
 * through its handshake, which is the state channel_share_waits_for_the_whole_table() is about
 * and the state nothing else here means to test.
 */
static void seed_radio(struct mesh_radio_settings *settings) {
    mesh_radio_settings_reset(settings);
    for (size_t slot = 0; slot < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++slot) {
        set_slot(settings, slot, "", 0U, meshtastic_Channel_Role_DISABLED);
    }
    set_slot(settings, 0U, "LongFast", 1U, meshtastic_Channel_Role_PRIMARY);
    set_slot(settings, 1U, "Trail", 2U, meshtastic_Channel_Role_SECONDARY);
    settings->has_lora = true;
    settings->lora.use_preset = true;
    settings->lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    settings->has_owner = true;
}

MESH_TEST_CASE(channel_share_builds_the_set_this_radio_is_on, unit) {
    struct mesh_radio_settings settings;
    seed_radio(&settings);
    /* A disabled slot between the two that count: it is dropped rather than carried as a hole,
       because a hole in this radio's table says nothing about the reader's. */
    set_slot(&settings, 3U, "Old", 3U, meshtastic_Channel_Role_DISABLED);
    set_slot(&settings, 4U, "Ops", 4U, meshtastic_Channel_Role_SECONDARY);

    meshtastic_ChannelSet set;
    MESH_TEST_FAIL_IF(mesh_channel_share_build(&settings, &set) != 3U,
                      "the primary and both secondaries were not collected");
    MESH_TEST_FAIL_IF(strcmp(set.settings[0].name, "LongFast") != 0,
                      "the primary is not first in the set");
    MESH_TEST_FAIL_IF(strcmp(set.settings[1].name, "Trail") != 0 ||
                          strcmp(set.settings[2].name, "Ops") != 0,
                      "the secondaries came out in the wrong order");
    MESH_TEST_FAIL_IF(!set.has_lora_config ||
                          set.lora_config.region != meshtastic_Config_LoRaConfig_RegionCode_US,
                      "the LoRa config was not carried");

    char url[MESH_CHANNEL_URL_MAX];
    MESH_TEST_FAIL_IF(mesh_channel_share_url(&settings, url, sizeof url) == 0U,
                      "the share URL was not built");

    /* Without a primary there is nothing to share: a set whose first entry is a secondary is a
       set every reader would join under the wrong role. */
    struct mesh_radio_settings bare;
    mesh_radio_settings_reset(&bare);
    set_slot(&bare, 1U, "Trail", 2U, meshtastic_Channel_Role_SECONDARY);
    MESH_TEST_FAIL_IF(mesh_channel_share_build(&bare, &set) != 0U,
                      "a radio with no primary produced a set");
    MESH_TEST_FAIL_IF(mesh_channel_share_url(&bare, url, sizeof url) != 0U || url[0] != '\0',
                      "a radio with no primary produced a link");
    record_success(test_name);
}

MESH_TEST_CASE(channel_import_replaces_the_table, unit) {
    struct mesh_radio_settings settings;
    seed_radio(&settings);

    meshtastic_ChannelSet set = meshtastic_ChannelSet_init_zero;
    fill_channel(&set.settings[0], "Rally", 9U);
    set.settings_count = 1U;
    set.has_lora_config = true;
    set.lora_config.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;

    struct mesh_channel_import_plan plan;
    MESH_TEST_FAIL_IF(!mesh_channel_import_plan(&settings, &set, false, &plan),
                      "the plan was refused");
    /* Slot 0 becomes Rally and slot 1 is switched off: two writes, and the LoRa config after
       them. Slots 2 upwards are already disabled and are left alone. */
    MESH_TEST_FAIL_IF(plan.writes != 2U, "the replace did not plan the two channel writes");
    MESH_TEST_FAIL_IF(!plan.replaces_primary, "replacing the primary was not reported");
    MESH_TEST_FAIL_IF(!plan.writes_lora, "the LoRa config write was not reported");
    MESH_TEST_FAIL_IF(plan.channels != 1U || plan.full, "the plan miscounted the link");

    const int queued = mesh_channel_share_queue_import(&settings, &set, false);
    MESH_TEST_FAIL_IF(queued <= 0, "the import queued nothing");
    MESH_TEST_FAIL_IF(!mesh_radio_settings_write_pending(&settings),
                      "the queue does not report a write in flight");

    /* Importing what the radio is already on is a no-op rather than a reboot: the same link
       scanned twice must not cost the mesh a restart. */
    struct mesh_radio_settings same;
    seed_radio(&same);
    meshtastic_ChannelSet mine;
    MESH_TEST_FAIL_IF(mesh_channel_share_build(&same, &mine) == 0U, "the set did not build");
    MESH_TEST_FAIL_IF(!mesh_channel_import_plan(&same, &mine, false, &plan), "the plan refused");
    MESH_TEST_FAIL_IF(plan.writes != 0U, "re-importing this radio's own set planned writes");
    MESH_TEST_FAIL_IF(plan.replaces_primary, "re-importing its own set claimed a new primary");
    MESH_TEST_FAIL_IF(plan.writes_lora,
                      "re-importing this radio's own set planned a LoRa write, which is the "
                      "reboot the no-op is supposed to avoid");
    MESH_TEST_FAIL_IF(mesh_channel_share_queue_import(&same, &mine, false) != 0,
                      "re-importing this radio's own set queued something");
    record_success(test_name);
}

MESH_TEST_CASE(channel_import_add_keeps_what_is_there, unit) {
    struct mesh_radio_settings settings;
    seed_radio(&settings);

    meshtastic_ChannelSet set = meshtastic_ChannelSet_init_zero;
    /* Their primary and a secondary. Their primary is theirs, not ours: it lands as a
       secondary in a free slot and this radio keeps the primary it had. */
    fill_channel(&set.settings[0], "Rally", 9U);
    fill_channel(&set.settings[1], "Trail", 2U); /* already held: skipped */
    set.settings_count = 2U;
    set.has_lora_config = true;

    struct mesh_channel_import_plan plan;
    MESH_TEST_FAIL_IF(!mesh_channel_import_plan(&settings, &set, true, &plan), "the plan refused");
    MESH_TEST_FAIL_IF(plan.writes != 1U, "an add wrote something other than the one new channel");
    MESH_TEST_FAIL_IF(plan.replaces_primary, "an add claimed to replace the primary");
    MESH_TEST_FAIL_IF(plan.writes_lora,
                      "an add took the sender's LoRa config, which would move this radio off "
                      "every channel it already had");

    /* A full table has nowhere to put another channel, and says so rather than dropping one
       quietly. */
    struct mesh_radio_settings full;
    seed_radio(&full);
    for (size_t slot = 1U; slot < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++slot) {
        set_slot(&full, slot, "Busy", (uint8_t)slot, meshtastic_Channel_Role_SECONDARY);
    }
    MESH_TEST_FAIL_IF(!mesh_channel_import_plan(&full, &set, true, &plan), "the plan refused");
    MESH_TEST_FAIL_IF(!plan.full, "a full channel table was not reported");
    MESH_TEST_FAIL_IF(plan.writes != 0U, "a full channel table planned a write anyway");
    record_success(test_name);
}

/*
 * A channel table that is still arriving is not a channel table.
 *
 * The slots come back one admin reply at a time, so for the first few seconds of every connect
 * the radio has told us about some of them and not the others - and "not arrived" is
 * indistinguishable from "disabled" to anything reading the array. Both directions get that
 * wrong in a way the user pays for:
 *
 *   - Sharing half a set is a QR code that joins somebody to a mesh missing its secondaries,
 *     and without the LoRa config it joins them on the *default* modem settings, where they
 *     hear nothing at all. That failure looks exactly like a mistyped key.
 *   - Importing against half a table treats every unseen slot as free: an add lands on top of a
 *     secondary that had not arrived, and a replace leaves an old channel enabled beside the
 *     new set.
 *
 * So both halves wait for the whole table and the LoRa config, and this is the case that says
 * so. It is a *timing* bug, which is the kind a test has to be written for deliberately: every
 * other case here starts from a radio that has finished answering.
 */
MESH_TEST_CASE(channel_share_waits_for_the_whole_table, unit) {
    struct mesh_radio_settings settings;
    seed_radio(&settings);
    /* seed_radio() leaves a complete radio; take one slot back out to make it mid-sync. */
    settings.has_channel[5] = false;

    meshtastic_ChannelSet set;
    char url[MESH_CHANNEL_URL_MAX];
    MESH_TEST_FAIL_IF(mesh_channel_share_build(&settings, &set) != 0U,
                      "a half-arrived table produced a set to share");
    MESH_TEST_FAIL_IF(mesh_channel_share_url(&settings, url, sizeof url) != 0U || url[0] != '\0',
                      "a half-arrived table produced a link");

    /* And the same for the LoRa config, which decides whether a joiner can hear the mesh at
       all: a link without it is worse than no link. */
    struct mesh_radio_settings no_lora;
    seed_radio(&no_lora);
    no_lora.has_lora = false;
    MESH_TEST_FAIL_IF(mesh_channel_share_build(&no_lora, &set) != 0U,
                      "a radio that has not sent its LoRa config produced a set to share");

    /* Importing is refused for the same reason from the other side: an unseen slot reads as a
       free one, so an add would land on top of a channel and a replace would leave one on. */
    meshtastic_ChannelSet theirs = meshtastic_ChannelSet_init_zero;
    fill_channel(&theirs.settings[0], "Rally", 9U);
    theirs.settings_count = 1U;
    struct mesh_channel_import_plan plan;
    MESH_TEST_FAIL_IF(mesh_channel_import_plan(&settings, &theirs, false, &plan),
                      "a half-arrived table was planned against");
    MESH_TEST_FAIL_IF(mesh_channel_share_queue_import(&settings, &theirs, false) >= 0,
                      "a half-arrived table was written to");
    MESH_TEST_FAIL_IF(mesh_channel_share_queue_import(&settings, &theirs, true) >= 0,
                      "a half-arrived table was added to");

    /* The slot arriving is what opens both halves. */
    settings.has_channel[5] = true;
    MESH_TEST_FAIL_IF(mesh_channel_share_build(&settings, &set) == 0U,
                      "the complete table did not produce a set");
    MESH_TEST_FAIL_IF(!mesh_channel_import_plan(&settings, &theirs, false, &plan),
                      "the complete table was not planned against");
    record_success(test_name);
}

MESH_TEST_CASE(channel_import_refuses_without_a_radio, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);
    meshtastic_ChannelSet set = meshtastic_ChannelSet_init_zero;
    fill_channel(&set.settings[0], "Rally", 9U);
    set.settings_count = 1U;

    struct mesh_channel_import_plan plan;
    MESH_TEST_FAIL_IF(mesh_channel_import_plan(&settings, &set, false, &plan),
                      "a radio that has said nothing was planned against");
    MESH_TEST_FAIL_IF(mesh_channel_share_queue_import(&settings, &set, false) >= 0,
                      "an import was queued against a radio that has said nothing");

    seed_radio(&settings);
    meshtastic_ChannelSet empty = meshtastic_ChannelSet_init_zero;
    MESH_TEST_FAIL_IF(mesh_channel_import_plan(&settings, &empty, false, &plan),
                      "an empty set was planned");
    MESH_TEST_FAIL_IF(mesh_channel_share_queue_import(&settings, &empty, false) >= 0,
                      "an empty set was queued");
    record_success(test_name);
}

/* ---- the two screens --------------------------------------------------------------------- */

/*
 * The whole press-by-press path, because the two halves of this feature are almost entirely
 * navigation: a row that opens a picture, and a row that opens a keyboard whose Done leads to a
 * sheet whose answer is the only thing that reaches the radio.
 */
MESH_TEST_CASE(channel_share_rows_drive_the_two_screens, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_channels = true;
    /* The radio has finished answering, which is what both sharing rows wait for. */
    settings.channels_settled = true;
    settings.channels[0].present = true;
    settings.channels[0].role = 1U;
    settings.channels[0].psk_len = 1U;
    settings.channels[0].psk[0] = 1U;
    /* The link the publish boundary would have filled in, which is what the share row keys off
       and what the screen draws. */
    meshtastic_ChannelSet mine = meshtastic_ChannelSet_init_zero;
    fill_channel(&mine.settings[0], "LongFast", 1U);
    mine.settings_count = 1U;
    (void)mesh_channel_url_encode(&mine, false, settings.share_url, sizeof settings.share_url);
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
    mesh_test_settings_open(&store, MESH_UI_SETTINGS_CHANNELS);

    /* One slot, then share, then import. */
    if (mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 3U) {
        failure = "a radio with a link should offer both sharing rows";
        goto cleanup;
    }
    /*
     * And neither of them before the radio has finished answering, which is the window both
     * rows would otherwise be wrong in - `has_channels` is true from the first channel reply.
     */
    {
        struct mesh_ui_settings syncing = settings;
        syncing.channels_settled = false;
        mesh_ui_store_set_settings(&store, &syncing);
        if (mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 1U) {
            failure = "a table that is still arriving should offer neither sharing row";
            goto cleanup;
        }
        mesh_ui_store_set_settings(&store, &settings);
    }

    /* ---- share: a row that opens a picture and nothing else ---- */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.share_open || action.type != MESH_UI_ACTION_NONE) {
        failure = "A on the share row should open the sheet and ask the app for nothing";
        goto cleanup;
    }
    /* Nothing on it moves, so nothing but B does anything - a thumb on the pad must not dismiss
       a code somebody is scanning. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.share_open) {
        failure = "the share sheet should ignore every key but B";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.share_open) {
        failure = "B should close the share sheet";
        goto cleanup;
    }

    /* What the screen puts under the code, read off the same characters it draws. */
    char summary[96];
    if (!mesh_ui_channel_share_summary(settings.share_url, summary, sizeof summary) ||
        strstr(summary, "1 channel") == NULL) {
        failure = "the share summary should count the link's channels";
        goto cleanup;
    }
    if (mesh_ui_channel_share_summary("", summary, sizeof summary) || summary[0] != '\0') {
        failure = "an empty link should have no summary";
        goto cleanup;
    }

    /* ---- import: a row, a keyboard, a sheet ---- */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.keyboard_open || !store.nav.keyboard_channel_url) {
        failure = "A on the import row should open the link keyboard";
        goto cleanup;
    }

    /*
     * Done on something that is not a link leaves the user on the keyboard with what they
     * typed: two hundred characters of base64 are not worth throwing away over one of them.
     *
     * The draft is filled in rather than typed. Every press between here and there is the
     * keyboard's own and is covered where the keyboard is; what this case is about is what Done
     * does with what is in the buffer, and walking a d-pad over sixty characters of base64
     * would test the grid twice and this once.
     */
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "not a link");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (!store.nav.keyboard_open || store.nav.confirm_open || store.nav.toast[0] == '\0') {
        failure = "a link that does not parse should stay on the keyboard and say so";
        goto cleanup;
    }

    meshtastic_ChannelSet theirs = meshtastic_ChannelSet_init_zero;
    fill_channel(&theirs.settings[0], "Rally", 9U);
    theirs.settings_count = 1U;
    char link[MESH_CHANNEL_URL_MAX];
    if (mesh_channel_url_encode(&theirs, false, link, sizeof link) == 0U) {
        failure = "the link did not encode";
        goto cleanup;
    }
    if (!inkwell_str_copy(store.nav.draft, sizeof store.nav.draft, link)) {
        failure = "the link did not fit the draft the keyboard fills";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
    if (store.nav.keyboard_open || !store.nav.confirm_open ||
        store.nav.confirm_action != (uint8_t)MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS ||
        store.nav.confirm_cursor != 1U) {
        failure = "a link should close the keyboard and raise the sheet, on Cancel";
        goto cleanup;
    }

    /* The sheet names the mesh being joined rather than counting anything, because the name is
       the only part of a link a person can read. */
    char headline[96];
    char body[256];
    if (!mesh_ui_channel_import_sheet(store.nav.channel_url, headline, sizeof headline, body,
                                      sizeof body) ||
        strstr(headline, "Rally") == NULL || strstr(body, "1 channel") == NULL) {
        failure = "the import sheet should name the channel being joined";
        goto cleanup;
    }

    /* And the answer is the only thing that reaches the app - carrying the link itself, so the
       app can parse it against whatever the radio's table has become since. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action); /* onto the accept button */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_IMPORT_CHANNELS ||
        strcmp(action.text, link) != 0) {
        failure = "the sheet's accept should hand the app the link";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
