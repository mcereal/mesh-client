#define _POSIX_C_SOURCE 200809L

/*
 * The backup file and the directory of them: what a radio was set to, kept on the card.
 *
 * The claim everything else rests on is the first pair of cases: a backup reads back exactly as
 * it was written, and a backup that did not finish writing is refused rather than read as a
 * radio with fewer channels than it had. The rest is the store's bookkeeping - one directory per
 * radio, files in the order they were taken whatever the clock said, automatic ones pruned and
 * manual ones kept.
 */

#include "framework/mesh_test.h"
#include "support/backup_fixture.h"
#include "support/fs_fixture.h"

#include "inkwell/codec/sha256.h"
#include "mesh/core/radio_backup.h"
#include "mesh/core/radio_backup_meshtastic.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct mesh_radio_backup g_backup;
static struct mesh_radio_backup g_read;

static bool backup_tempdir(char *dir, size_t dir_len) {
    snprintf(dir, dir_len, "/tmp/mesh_backup_XXXXXX");
    return mkdtemp(dir) != NULL;
}

/* A backup with a header in every field and a few sections, one of them the largest allowed. */
static void backup_fill(struct mesh_radio_backup *backup, uint8_t reason) {
    mesh_radio_backup_reset(backup);
    struct mesh_radio_backup_header *header = &backup->header;
    header->protocol = MESH_RADIO_BACKUP_MESHTASTIC;
    header->reason = reason;
    header->saved_at = 1767225600U;
    header->node_id = 0xa1b2c3d4U;
    snprintf(header->device, sizeof header->device, "AA:BB:CC:DD:EE:FF");
    /* An '=' and a newline, which the record file has to escape and give back. */
    snprintf(header->name, sizeof header->name, "Base=camp\nnorth");
    snprintf(header->model, sizeof header->model, "HELTEC_V3");
    snprintf(header->firmware, sizeof header->firmware, "2.7.15.567b8ea");
    snprintf(header->region, sizeof header->region, "US");
    header->has_radio = true;
    header->frequency_khz = 906875U;
    header->bandwidth_hz = 250000U;
    header->spreading_factor = 11U;
    header->coding_rate = 5U;
    header->tx_power_dbm = -4;
    header->channel_count = 3U;
    snprintf(header->channel_names[0], sizeof header->channel_names[0], "LongFast");
    snprintf(header->channel_names[1], sizeof header->channel_names[1], "a,b");
    snprintf(header->channel_names[2], sizeof header->channel_names[2], "%s", "");
    header->has_nodes_heard = true;
    header->nodes_heard = 42U;

    const uint8_t small[] = {0x08, 0x01, 0x10, 0x00};
    (void)mesh_radio_backup_add(backup, 1U, small, sizeof small);
    uint8_t large[MESH_RADIO_BACKUP_SECTION_MAX];
    for (size_t i = 0; i < sizeof large; ++i) {
        large[i] = (uint8_t)(i * 7U);
    }
    (void)mesh_radio_backup_add(backup, 2U, large, sizeof large);
    (void)mesh_radio_backup_add(backup, 1U, small, 1U);
}

static bool backup_headers_equal(const struct mesh_radio_backup_header *a,
                                 const struct mesh_radio_backup_header *b) {
    if (a->protocol != b->protocol || a->reason != b->reason || a->saved_at != b->saved_at ||
        a->node_id != b->node_id || strcmp(a->device, b->device) != 0 ||
        strcmp(a->name, b->name) != 0 || strcmp(a->model, b->model) != 0 ||
        strcmp(a->firmware, b->firmware) != 0 || strcmp(a->region, b->region) != 0 ||
        strcmp(a->preset, b->preset) != 0 || a->has_radio != b->has_radio ||
        a->frequency_khz != b->frequency_khz || a->bandwidth_hz != b->bandwidth_hz ||
        a->spreading_factor != b->spreading_factor || a->coding_rate != b->coding_rate ||
        a->tx_power_dbm != b->tx_power_dbm || a->channel_count != b->channel_count ||
        a->has_nodes_heard != b->has_nodes_heard || a->nodes_heard != b->nodes_heard ||
        a->has_contacts != b->has_contacts || a->contacts != b->contacts) {
        return false;
    }
    for (size_t i = 0; i < a->channel_count; ++i) {
        if (strcmp(a->channel_names[i], b->channel_names[i]) != 0) {
            return false;
        }
    }
    return true;
}

static long backup_file_size(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 ? (long)info.st_size : -1L;
}

/* Rewrites `path` with its first `keep` bytes, as a write cut off by the power would leave it. */
static bool backup_truncate(const char *path, long keep) { return truncate(path, keep) == 0; }

MESH_TEST_CASE(radio_backup_file_round_trips, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!backup_tempdir(dir, sizeof dir), "mkdtemp failed");
    char path[128];
    snprintf(path, sizeof path, "%s/one.backup", dir);

    backup_fill(&g_backup, MESH_RADIO_BACKUP_BEFORE_WRITE);
    const int written = mesh_radio_backup_write_file(&g_backup, path);
    const int read = mesh_radio_backup_read_file(&g_read, path);
    const bool same_header = backup_headers_equal(&g_backup.header, &g_read.header);
    const bool same_payload = mesh_radio_backup_same_payload(&g_backup, &g_read);
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(written != 0, "write failed");
    MESH_TEST_FAIL_IF(read != 0, "read of a whole file failed");
    MESH_TEST_FAIL_IF(!same_header, "the header did not come back as written");
    MESH_TEST_FAIL_IF(!same_payload, "the sections did not come back as written");
    MESH_TEST_FAIL_IF(g_read.section_count != 3U, "three sections went in");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_cut_short_is_refused_not_misread, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!backup_tempdir(dir, sizeof dir), "mkdtemp failed");
    char path[128];
    snprintf(path, sizeof path, "%s/cut.backup", dir);
    backup_fill(&g_backup, MESH_RADIO_BACKUP_MANUAL);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_radio_backup_write_file(&g_backup, path) != 0,
                              mesh_test_remove_tree(dir), "write failed");
    const long size = backup_file_size(path);

    /* Cut at every tenth of the file, and just before the last byte: each is a pulled battery
       somewhere in the write, and none of them may read back as a smaller radio. */
    bool refused = true;
    bool reset = true;
    for (int tenth = 1; tenth <= 10 && size > 0; ++tenth) {
        const long keep = tenth == 10 ? size - 1L : size * tenth / 10L;
        mesh_radio_backup_write_file(&g_backup, path);
        backup_truncate(path, keep);
        const int result = mesh_radio_backup_read_file(&g_read, path);
        refused = refused && result == -EBADMSG;
        reset = reset && g_read.section_count == 0U && g_read.header.node_id == 0U;
    }
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(size <= 0, "no file was written");
    MESH_TEST_FAIL_IF(!refused, "a truncated backup read as something other than -EBADMSG");
    MESH_TEST_FAIL_IF(!reset, "a refused read left part of a backup behind");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_edited_byte_fails_its_digest, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!backup_tempdir(dir, sizeof dir), "mkdtemp failed");
    char path[128];
    snprintf(path, sizeof path, "%s/edit.backup", dir);
    backup_fill(&g_backup, MESH_RADIO_BACKUP_MANUAL);
    mesh_radio_backup_write_file(&g_backup, path);

    /* Change one hex digit inside the first section. */
    FILE *file = fopen(path, "r+");
    char text[8192];
    const size_t len = file != NULL ? fread(text, 1U, sizeof text - 1U, file) : 0U;
    text[len] = '\0';
    char *section = strstr(text, "section=1,");
    if (section != NULL) {
        char *digit = section + strlen("section=1,");
        *digit = *digit == '0' ? '1' : '0';
        fseek(file, 0L, SEEK_SET);
        fwrite(text, 1U, len, file);
    }
    if (file != NULL) {
        fclose(file);
    }
    const int result = mesh_radio_backup_read_file(&g_read, path);
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(section == NULL, "fixture: no section line found");
    MESH_TEST_FAIL_IF(result != -EBADMSG, "an edited backup was accepted");
    record_success(test_name);
}

/* Writes a file by hand, so a case can say exactly what is in it. */
static bool backup_write_raw(const char *path, const char *const *lines, size_t count) {
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        fprintf(file, "%s\n", lines[i]);
    }
    return fclose(file) == 0;
}

MESH_TEST_CASE(radio_backup_newer_format_is_refused, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!backup_tempdir(dir, sizeof dir), "mkdtemp failed");
    char path[128];
    snprintf(path, sizeof path, "%s/new.backup", dir);
    char format[32];
    snprintf(format, sizeof format, "format=%u", MESH_RADIO_BACKUP_FORMAT + 1U);
    const char *lines[] = {format, "protocol=meshtastic", "node=00000001", "sha256=00"};
    backup_write_raw(path, lines, sizeof lines / sizeof lines[0]);
    const int result = mesh_radio_backup_read_file(&g_read, path);
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(result != -EPROTO, "a later format read as anything but -EPROTO");
    record_success(test_name);
}

/* The same, sealed: the digest a writer would have put under exactly these lines. */
static bool backup_write_sealed(const char *path, const char *const *lines, size_t count) {
    struct inkwell_sha256 digest;
    inkwell_sha256_init(&digest);
    for (size_t i = 0; i < count; ++i) {
        inkwell_sha256_update(&digest, lines[i], strlen(lines[i]));
        inkwell_sha256_update(&digest, "\n", 1U);
    }
    uint8_t sum[INKWELL_SHA256_DIGEST_LEN];
    inkwell_sha256_final(&digest, sum);
    char hex[INKWELL_SHA256_DIGEST_LEN * 2U + 1U];
    inkwell_sha256_hex(sum, hex, sizeof hex);
    char seal[sizeof hex + 8U];
    snprintf(seal, sizeof seal, "sha256=%s", hex);
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        fprintf(file, "%s\n", lines[i]);
    }
    fprintf(file, "%s\n", seal);
    return fclose(file) == 0;
}

MESH_TEST_CASE(radio_backup_unknown_key_is_skipped_and_still_vouched_for, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!backup_tempdir(dir, sizeof dir), "mkdtemp failed");
    char path[128];
    snprintf(path, sizeof path, "%s/later.backup", dir);

    /* A later build of this format added a key. This build skips it - but the digest covers
       it, so it is read, not ignored, on the way to agreeing with the seal. */
    const char *lines[] = {"format=1",      "protocol=meshcore", "reason=manual",
                           "node=0000beef", "future_field=7",    "section=3,0a0b"};
    const bool sealed = backup_write_sealed(path, lines, sizeof lines / sizeof lines[0]);
    const int accepted = mesh_radio_backup_read_file(&g_read, path);
    const bool fields = g_read.header.protocol == MESH_RADIO_BACKUP_MESHCORE &&
                        g_read.header.node_id == 0xbeefU && g_read.section_count == 1U;

    /* And a line slipped in after sealing is one nobody vouched for. */
    FILE *file = fopen(path, "a");
    if (file != NULL) {
        fputs("section=3,ff\n", file);
        fclose(file);
    }
    const int appended = mesh_radio_backup_read_file(&g_read, path);
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(!sealed, "fixture: could not write the file");
    MESH_TEST_FAIL_IF(accepted != 0, "a sealed file with an unknown key was refused");
    MESH_TEST_FAIL_IF(!fields, "the known fields around the unknown key were lost");
    MESH_TEST_FAIL_IF(appended != -EBADMSG, "a record after the digest was accepted");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_rejects_what_it_cannot_name, unit) {
    backup_fill(&g_backup, MESH_RADIO_BACKUP_MANUAL);
    g_backup.header.node_id = 0U;
    MESH_TEST_FAIL_IF(mesh_radio_backup_write_file(&g_backup, "/tmp/never.backup") != -EINVAL,
                      "a backup of no node was written");
    backup_fill(&g_backup, MESH_RADIO_BACKUP_MANUAL);
    g_backup.header.protocol = MESH_RADIO_BACKUP_PROTOCOL_NONE;
    MESH_TEST_FAIL_IF(mesh_radio_backup_write_file(&g_backup, "/tmp/never.backup") != -EINVAL,
                      "a backup of no protocol was written");

    uint8_t big[MESH_RADIO_BACKUP_SECTION_MAX + 1U] = {0};
    MESH_TEST_FAIL_IF(mesh_radio_backup_add(&g_backup, 1U, big, sizeof big) != -EINVAL,
                      "an oversized section went in");
    mesh_radio_backup_reset(&g_backup);
    int result = 0;
    size_t added = 0U;
    while ((result = mesh_radio_backup_add(&g_backup, 1U, big, MESH_RADIO_BACKUP_SECTION_MAX)) ==
           0) {
        ++added;
    }
    MESH_TEST_FAIL_IF(result != -ENOSPC, "a full backup said something other than -ENOSPC");
    MESH_TEST_FAIL_IF(added * MESH_RADIO_BACKUP_SECTION_MAX > MESH_RADIO_BACKUP_PAYLOAD_MAX,
                      "more went in than the payload holds");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_store_orders_by_sequence_not_clock, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!backup_tempdir(dir, sizeof dir), "mkdtemp failed");
    char root[96];
    snprintf(root, sizeof root, "%s/backups", dir);
    struct mesh_radio_backup_store store;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_radio_backup_store_init(&store, root) != 0,
                              mesh_test_remove_tree(dir), "store init failed");

    /* A handheld with no clock: every backup says 0, and they must still list in order. */
    const uint8_t reasons[] = {MESH_RADIO_BACKUP_FIRST_CONNECT, MESH_RADIO_BACKUP_MANUAL,
                               MESH_RADIO_BACKUP_BEFORE_WRITE};
    bool saved = true;
    for (size_t i = 0; i < sizeof reasons; ++i) {
        backup_fill(&g_backup, reasons[i]);
        g_backup.header.saved_at = 0U;
        saved = saved && mesh_radio_backup_store_save(&store, &g_backup, NULL) == 0;
    }
    struct mesh_radio_backup_entry entries[8];
    const int total = mesh_radio_backup_store_list(&store, 0xa1b2c3d4U, entries, 8U);
    const int other = mesh_radio_backup_store_list(&store, 0x01020304U, entries + 4, 4U);
    const int loaded =
        total > 0 ? mesh_radio_backup_store_load(&store, 0xa1b2c3d4U, &entries[0], &g_read) : -1;
    struct mesh_radio_backup_entry escape = entries[0];
    snprintf(escape.file, sizeof escape.file, "../x.backup");
    const int escaped = mesh_radio_backup_store_load(&store, 0xa1b2c3d4U, &escape, &g_read);
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(!saved, "a save failed");
    MESH_TEST_FAIL_IF(total != 3, "three backups were saved");
    MESH_TEST_FAIL_IF(other != 0, "a radio with no backups listed some");
    MESH_TEST_FAIL_IF(entries[0].sequence != 3U || entries[2].sequence != 1U,
                      "not listed newest first");
    MESH_TEST_FAIL_IF(entries[0].reason != MESH_RADIO_BACKUP_BEFORE_WRITE ||
                          entries[1].reason != MESH_RADIO_BACKUP_MANUAL ||
                          entries[2].reason != MESH_RADIO_BACKUP_FIRST_CONNECT,
                      "the reason did not come back from the name");
    MESH_TEST_FAIL_IF(strcmp(entries[0].file, "00000003.before_write.backup") != 0,
                      "file name is not sequence.reason.backup");
    MESH_TEST_FAIL_IF(loaded != 0 || g_read.header.reason != MESH_RADIO_BACKUP_BEFORE_WRITE,
                      "the newest did not load");
    MESH_TEST_FAIL_IF(escaped != -EINVAL, "a file name reaching out of the directory was opened");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_store_lists_radios_and_removes_one_backup, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!backup_tempdir(dir, sizeof dir), "mkdtemp failed");
    char root[96];
    snprintf(root, sizeof root, "%s/backups", dir);
    struct mesh_radio_backup_store store;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_radio_backup_store_init(&store, root) != 0,
                              mesh_test_remove_tree(dir), "store init failed");

    backup_fill(&g_backup, MESH_RADIO_BACKUP_MANUAL);
    bool saved = mesh_radio_backup_store_save(&store, &g_backup, NULL) == 0;
    saved = saved && mesh_radio_backup_store_save(&store, &g_backup, NULL) == 0;
    g_backup.header.node_id = 0x01020304U;
    saved = saved && mesh_radio_backup_store_save(&store, &g_backup, NULL) == 0;
    /* A stray directory that is not a radio, and one that is named like one but is empty. */
    char stray[160];
    snprintf(stray, sizeof stray, "%s/notes", root);
    mkdir(stray, 0700);
    snprintf(stray, sizeof stray, "%s/0000beef", root);
    mkdir(stray, 0700);

    uint32_t radios[4] = {0};
    const int count = mesh_radio_backup_store_radios(&store, radios, 4U);
    const int removed = mesh_radio_backup_store_remove(&store, 0xa1b2c3d4U, 1U);
    const int again = mesh_radio_backup_store_remove(&store, 0xa1b2c3d4U, 1U);
    struct mesh_radio_backup_entry left[4];
    const int remaining = mesh_radio_backup_store_list(&store, 0xa1b2c3d4U, left, 4U);
    const int second = mesh_radio_backup_store_remove(&store, 0x01020304U, 1U);
    const int after = mesh_radio_backup_store_radios(&store, radios + 2, 2U);
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(!saved, "a save failed");
    MESH_TEST_FAIL_IF(count != 2 || !((radios[0] == 0xa1b2c3d4U && radios[1] == 0x01020304U) ||
                                      (radios[1] == 0xa1b2c3d4U && radios[0] == 0x01020304U)),
                      "the two radios were not listed, or something else was");
    MESH_TEST_FAIL_IF(removed != 0 || again != -ENOENT, "a backup did not remove exactly once");
    MESH_TEST_FAIL_IF(remaining != 1 || left[0].sequence != 2U, "the wrong backup went");
    /* A radio whose last backup went is no longer a radio with backups. */
    MESH_TEST_FAIL_IF(second != 0 || after != 1 || radios[2] != 0xa1b2c3d4U,
                      "a radio with no backups left was still listed");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_store_prunes_automatic_and_keeps_manual, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!backup_tempdir(dir, sizeof dir), "mkdtemp failed");
    struct mesh_radio_backup_store store;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_radio_backup_store_init(&store, dir) != 0,
                              mesh_test_remove_tree(dir), "store init failed");
    store.keep_automatic = 3U;

    /* Two manual ones first - the oldest in the directory - then eight automatic. */
    backup_fill(&g_backup, MESH_RADIO_BACKUP_MANUAL);
    mesh_radio_backup_store_save(&store, &g_backup, NULL);
    mesh_radio_backup_store_save(&store, &g_backup, NULL);
    for (int i = 0; i < 8; ++i) {
        backup_fill(&g_backup, i % 2 == 0 ? MESH_RADIO_BACKUP_BEFORE_WRITE
                                          : MESH_RADIO_BACKUP_BEFORE_FIRMWARE);
        mesh_radio_backup_store_save(&store, &g_backup, NULL);
    }
    struct mesh_radio_backup_entry entries[16];
    const int total = mesh_radio_backup_store_list(&store, 0xa1b2c3d4U, entries, 16U);
    size_t manual = 0U;
    uint32_t oldest_automatic = UINT32_MAX;
    for (int i = 0; i < total && i < 16; ++i) {
        if (entries[i].reason == MESH_RADIO_BACKUP_MANUAL) {
            ++manual;
        } else if (entries[i].sequence < oldest_automatic) {
            oldest_automatic = entries[i].sequence;
        }
    }
    /* A capped list keeps the newest. */
    struct mesh_radio_backup_entry two[2];
    const int capped = mesh_radio_backup_store_list(&store, 0xa1b2c3d4U, two, 2U);
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(total != 5, "expected two manual and three automatic backups");
    MESH_TEST_FAIL_IF(manual != 2U, "a manual backup was pruned");
    MESH_TEST_FAIL_IF(oldest_automatic != 8U, "the pruned ones were not the oldest");
    MESH_TEST_FAIL_IF(capped != 5 || two[0].sequence != 10U || two[1].sequence != 9U,
                      "a capped list did not keep the newest");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_store_without_a_directory_is_quiet, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!backup_tempdir(dir, sizeof dir), "mkdtemp failed");
    /* A file where the directory should be. */
    char blocked[96];
    snprintf(blocked, sizeof blocked, "%s/backups", dir);
    FILE *file = fopen(blocked, "w");
    if (file != NULL) {
        fclose(file);
    }
    struct mesh_radio_backup_store store;
    const int init = mesh_radio_backup_store_init(&store, blocked);
    backup_fill(&g_backup, MESH_RADIO_BACKUP_MANUAL);
    const int save = mesh_radio_backup_store_save(&store, &g_backup, NULL);
    const int list = mesh_radio_backup_store_list(&store, 0xa1b2c3d4U, NULL, 0U);
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(init == 0 || mesh_radio_backup_store_enabled(&store),
                      "a store over a file came up enabled");
    MESH_TEST_FAIL_IF(save != -ENODEV || list != -ENODEV, "a disabled store did something");
    record_success(test_name);
}

/* ---- a Meshtastic radio -------------------------------------------------------------------- */

static struct mesh_radio_settings g_settings;
static struct mesh_radio_settings g_restored;
static struct mesh_handshake_status g_status;

MESH_TEST_CASE(radio_backup_meshtastic_waits_for_a_whole_radio, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    MESH_TEST_FAIL_IF(!mesh_radio_backup_meshtastic_ready(&g_settings),
                      "fixture: a whole radio was not ready");

    g_settings.has_channel[7] = false;
    const int half = mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    MESH_TEST_FAIL_IF(half != -EAGAIN || g_backup.section_count != 0U,
                      "a radio still sending its channels was captured");

    mesh_test_backup_radio(&g_settings, &g_status);
    g_settings.has_lora = false;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup) !=
                          -EAGAIN,
                      "a radio with a Config section missing was captured");

    mesh_test_backup_radio(&g_settings, &g_status);
    g_settings.admin_dest = 0x12345678U;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup) !=
                          -EAGAIN,
                      "another node's settings were captured as this radio's");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_round_trips_through_the_card, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup) != 0,
                      "capture failed");
    g_backup.header.reason = MESH_RADIO_BACKUP_MANUAL;

    char dir[64];
    MESH_TEST_FAIL_IF(!backup_tempdir(dir, sizeof dir), "mkdtemp failed");
    char path[128];
    snprintf(path, sizeof path, "%s/mt.backup", dir);
    const int written = mesh_radio_backup_write_file(&g_backup, path);
    const int read = mesh_radio_backup_read_file(&g_read, path);
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(written != 0 || read != 0, "the file did not round-trip");

    const int restored = mesh_radio_backup_meshtastic_read(&g_read, &g_restored, NULL);
    MESH_TEST_FAIL_IF(restored != 0, "the sections did not decode");
    /* Every setting, but not the metadata: that describes the firmware rather than a choice
       anybody made, and it goes into the header instead. */
    bool channels = true;
    for (size_t i = 0; i < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++i) {
        channels = channels && g_restored.has_channel[i];
    }
    MESH_TEST_FAIL_IF(
        !channels || !g_restored.has_device || !g_restored.has_position || !g_restored.has_power ||
            !g_restored.has_network || !g_restored.has_display || !g_restored.has_lora ||
            !g_restored.has_bluetooth || !g_restored.has_security || !g_restored.has_owner,
        "what came back is not a whole radio");
    MESH_TEST_FAIL_IF(g_restored.has_metadata, "the metadata is the header's, not a setting");
    MESH_TEST_FAIL_IF(g_restored.lora.region != meshtastic_Config_LoRaConfig_RegionCode_EU_868 ||
                          g_restored.lora.modem_preset !=
                              meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST ||
                          g_restored.lora.hop_limit != 5U || g_restored.lora.tx_power != 17,
                      "the LoRa config changed on the way");
    MESH_TEST_FAIL_IF(g_restored.device.role != meshtastic_Config_DeviceConfig_Role_ROUTER,
                      "the device role changed on the way");
    MESH_TEST_FAIL_IF(strcmp(g_restored.owner.long_name, "Ridge relay") != 0,
                      "the owner changed on the way");
    MESH_TEST_FAIL_IF(g_restored.channels[2].role != meshtastic_Channel_Role_SECONDARY ||
                          strcmp(g_restored.channels[2].settings.name, "Ops") != 0 ||
                          g_restored.channels[2].settings.psk.size != 16U ||
                          g_restored.channels[2].settings.psk.bytes[15] != 0x42,
                      "a secondary channel and its key did not come back");
    MESH_TEST_FAIL_IF(!g_restored.has_telemetry ||
                          g_restored.telemetry.device_update_interval != 1800U,
                      "a module did not come back");
    MESH_TEST_FAIL_IF(!g_restored.has_canned_messages ||
                          strcmp(g_restored.canned_messages, "OK|On my way|Help") != 0,
                      "the canned messages did not come back");
    MESH_TEST_FAIL_IF(g_restored.has_ringtone, "a ringtone the radio never sent appeared");

    const struct mesh_radio_backup_header *header = &g_read.header;
    MESH_TEST_FAIL_IF(header->node_id != 0x0badcafeU || header->nodes_heard != 57U ||
                          !header->has_nodes_heard || header->has_contacts,
                      "the header did not name the radio");
    MESH_TEST_FAIL_IF(strcmp(header->name, "Ridge relay") != 0 ||
                          strcmp(header->firmware, "2.7.15.567b8ea") != 0 ||
                          header->model[0] == '\0',
                      "the header lost the radio's name, firmware or model");
    MESH_TEST_FAIL_IF(header->preset[0] == '\0' || header->region[0] == '\0' ||
                          header->bandwidth_hz != 0U || header->tx_power_dbm != 17,
                      "a preset radio's header should name the preset, not numbers");
    MESH_TEST_FAIL_IF(header->channel_count != 3U || strcmp(header->channel_names[2], "Ops") != 0,
                      "the header's channel list is wrong");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_never_keeps_the_private_key, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup) != 0,
                      "capture failed");
    g_backup.header.reason = MESH_RADIO_BACKUP_MANUAL;

    /* Not only absent after decoding: absent from the bytes on the card. */
    bool key_bytes = false;
    const uint8_t *data = NULL;
    const struct mesh_radio_backup_section *section = NULL;
    const uint8_t run[8] = {0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB};
    for (size_t i = 0; (section = mesh_radio_backup_section_at(&g_backup, i, &data)) != NULL; ++i) {
        for (size_t at = 0; at + sizeof run <= section->len; ++at) {
            key_bytes = key_bytes || memcmp(data + at, run, sizeof run) == 0;
        }
    }
    MESH_TEST_FAIL_IF(key_bytes, "the private key's bytes are in the backup");
    MESH_TEST_FAIL_IF(g_settings.security.private_key.size != 32U,
                      "capturing emptied the live settings' key");

    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_read(&g_backup, &g_restored, NULL) != 0,
                      "the sections did not decode");
    MESH_TEST_FAIL_IF(g_restored.security.private_key.size != 0U,
                      "a private key came back out of a backup");
    MESH_TEST_FAIL_IF(g_restored.security.public_key.size != 32U,
                      "the public key, which is not a secret, was dropped");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_keeps_a_fixed_position_only_when_fixed, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    g_status.node_count = 1U;
    g_status.nodes[0].node_id = 0x0badcafeU;
    g_status.nodes[0].position.valid = true;
    g_status.nodes[0].position.latitude_i = 473977000;
    g_status.nodes[0].position.longitude_i = 85456000;

    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    const size_t moving = mesh_radio_backup_count_tag(&g_backup, MESH_RADIO_BACKUP_MT_POSITION);

    g_settings.position.fixed_position = true;
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    const size_t fixed = mesh_radio_backup_count_tag(&g_backup, MESH_RADIO_BACKUP_MT_POSITION);
    meshtastic_Position position;
    const int read = mesh_radio_backup_meshtastic_read(&g_backup, &g_restored, &position);

    MESH_TEST_FAIL_IF(moving != 0U, "a GPS radio's current fix was kept as a setting");
    MESH_TEST_FAIL_IF(fixed != 1U, "a fixed position was not kept");
    MESH_TEST_FAIL_IF(read != 0 || position.latitude_i != 473977000 ||
                          position.longitude_i != 85456000,
                      "the fixed position did not come back");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_refuses_another_protocols_backup, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    g_backup.header.protocol = MESH_RADIO_BACKUP_MESHCORE;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_read(&g_backup, &g_restored, NULL) != -EPROTO,
                      "a MeshCore backup was read as Meshtastic settings");
    record_success(test_name);
}

/* ---- comparing ----------------------------------------------------------------------------- */

static struct mesh_radio_backup_diff g_diff;

MESH_TEST_CASE(radio_backup_meshtastic_diff_of_the_same_radio_is_empty, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup) != 0,
                      "capture failed");
    /* A second capture a day later: a new time, a new reason, the same radio. */
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_read) != 0,
                      "second capture failed");
    g_read.header.saved_at = 86400U;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_diff(&g_backup, &g_read, &g_diff) != 0,
                      "the diff failed");
    MESH_TEST_FAIL_IF(g_diff.count != 0U || g_diff.total != 0U,
                      "an unchanged radio was reported as changed");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_diff_names_the_one_field_changed, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    g_settings.lora.hop_limit = 3U;
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_read);
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_diff(&g_backup, &g_read, &g_diff) != 0,
                      "the diff failed");
    /* One field, not "the LoRa section": the reader wants the line, not the paragraph. */
    MESH_TEST_FAIL_IF(g_diff.count != 1U || g_diff.total != 1U, "not exactly one change");
    const struct mesh_radio_backup_change *change = &g_diff.changes[0];
    MESH_TEST_FAIL_IF(change->kind != MESH_RADIO_BACKUP_CHANGED ||
                          change->topic != MESH_RADIO_BACKUP_TOPIC_LORA ||
                          change->field != meshtastic_Config_LoRaConfig_hop_limit_tag,
                      "the change does not name the LoRa hop limit");
    MESH_TEST_FAIL_IF(change->before.kind != MESH_RADIO_BACKUP_VALUE_UINT ||
                          change->before.number != 5 || change->after.number != 3,
                      "the values are not the backup's and the radio's");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_diff_reaches_inside_a_channel, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    snprintf(g_settings.channels[2].settings.name, sizeof g_settings.channels[2].settings.name,
             "%s", "Field");
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_read);
    mesh_radio_backup_meshtastic_diff(&g_backup, &g_read, &g_diff);
    MESH_TEST_FAIL_IF(g_diff.count != 1U, "a renamed channel was not one change");
    const struct mesh_radio_backup_change *change = &g_diff.changes[0];
    MESH_TEST_FAIL_IF(change->topic != MESH_RADIO_BACKUP_TOPIC_CHANNEL || change->index != 2U ||
                          change->field != meshtastic_Channel_settings_tag * 100U +
                                               meshtastic_ChannelSettings_name_tag,
                      "the change does not name slot 2's name");
    MESH_TEST_FAIL_IF(strcmp(change->before.text, "Ops") != 0 ||
                          strcmp(change->after.text, "Field") != 0,
                      "the names are not the backup's and the radio's");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_diff_pairs_sections_by_what_they_are, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    /* The same radio with its canned messages gone: one section fewer, and every section after
       it one place earlier - which must not read as every one of them changed. */
    g_settings.has_canned_messages = false;
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_read);
    mesh_radio_backup_meshtastic_diff(&g_backup, &g_read, &g_diff);
    MESH_TEST_FAIL_IF(g_diff.count != 1U || g_diff.changes[0].kind != MESH_RADIO_BACKUP_REMOVED ||
                          g_diff.changes[0].topic != MESH_RADIO_BACKUP_TOPIC_CANNED,
                      "a missing section was not one removal");
    /* And the other way round is an addition. */
    mesh_radio_backup_meshtastic_diff(&g_read, &g_backup, &g_diff);
    MESH_TEST_FAIL_IF(g_diff.count != 1U || g_diff.changes[0].kind != MESH_RADIO_BACKUP_ADDED,
                      "a new section was not one addition");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_diff_counts_past_what_it_keeps, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    for (size_t i = 0; i < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++i) {
        g_settings.channels[i].role = meshtastic_Channel_Role_SECONDARY;
        g_settings.channels[i].has_settings = true;
        snprintf(g_settings.channels[i].settings.name, sizeof g_settings.channels[i].settings.name,
                 "n%zu", i);
        g_settings.channels[i].settings.channel_num = 40U + (uint32_t)i;
        g_settings.channels[i].settings.id = 70U + (uint32_t)i;
        g_settings.channels[i].settings.uplink_enabled =
            !g_settings.channels[i].settings.uplink_enabled;
        g_settings.channels[i].settings.downlink_enabled =
            !g_settings.channels[i].settings.downlink_enabled;
        g_settings.channels[i].settings.psk.size = 1U;
        g_settings.channels[i].settings.psk.bytes[0] = (uint8_t)(9U + i);
        g_settings.channels[i].settings.has_module_settings = true;
        g_settings.channels[i].settings.module_settings.position_precision = 13U;
        g_settings.channels[i].settings.module_settings.is_muted = true;
    }
    meshtastic_Config_LoRaConfig *lora = &g_settings.lora;
    lora->hop_limit = 1U;
    lora->tx_power = 3;
    lora->use_preset = !lora->use_preset;
    lora->bandwidth = 250U;
    lora->spread_factor = 9U;
    lora->coding_rate = 6U;
    lora->channel_num = 3U;
    lora->tx_enabled = !lora->tx_enabled;
    lora->override_duty_cycle = !lora->override_duty_cycle;
    lora->ignore_mqtt = !lora->ignore_mqtt;
    lora->config_ok_to_mqtt = !lora->config_ok_to_mqtt;
    lora->sx126x_rx_boosted_gain = !lora->sx126x_rx_boosted_gain;
    g_settings.display.screen_on_secs = 77U;
    g_settings.power.ls_secs = 3U;
    g_settings.power.min_wake_secs = 4U;
    g_settings.power.wait_bluetooth_secs = 5U;
    g_settings.bluetooth.fixed_pin = 123456U;
    g_settings.device.node_info_broadcast_secs = 999U;
    g_settings.position.position_broadcast_secs = 61U;
    g_settings.position.gps_update_interval = 62U;
    g_settings.display.flip_screen = !g_settings.display.flip_screen;
    snprintf(g_settings.owner.long_name, sizeof g_settings.owner.long_name, "%s", "Other");
    snprintf(g_settings.owner.short_name, sizeof g_settings.owner.short_name, "%s", "OTH");
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_read);
    mesh_radio_backup_meshtastic_diff(&g_backup, &g_read, &g_diff);
    MESH_TEST_FAIL_IF(g_diff.count != MESH_RADIO_BACKUP_DIFF_MAX ||
                          g_diff.total <= MESH_RADIO_BACKUP_DIFF_MAX,
                      "a long list was not kept to its bound with the rest counted");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_diff_refuses_another_protocol, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_read);
    g_read.header.protocol = MESH_RADIO_BACKUP_MESHCORE;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_diff(&g_backup, &g_read, &g_diff) != -EPROTO,
                      "a MeshCore backup was compared as Meshtastic");
    record_success(test_name);
}

/* ---- restoring ----------------------------------------------------------------------------- */

static struct mesh_admin_request g_writes[MESH_RADIO_SETTINGS_TRANSACTION_MAX];

MESH_TEST_CASE(radio_backup_meshtastic_plan_of_an_unchanged_radio_is_empty, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_plan(&g_backup, &g_settings, &g_status, g_writes,
                                                        MESH_RADIO_SETTINGS_TRANSACTION_MAX) != 0,
                      "a radio that matches its backup was given writes");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_plan_writes_only_what_differs, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    g_settings.lora.hop_limit = 3U;
    const int planned = mesh_radio_backup_meshtastic_plan(
        &g_backup, &g_settings, &g_status, g_writes, MESH_RADIO_SETTINGS_TRANSACTION_MAX);
    MESH_TEST_FAIL_IF(planned != 1, "not exactly one write for one changed section");
    MESH_TEST_FAIL_IF(
        g_writes[0].kind != MESH_ADMIN_SET_CONFIG ||
            g_writes[0].type != (uint32_t)meshtastic_AdminMessage_ConfigType_LORA_CONFIG ||
            g_writes[0].payload.config.which_payload_variant != meshtastic_Config_lora_tag ||
            g_writes[0].payload.config.payload_variant.lora.hop_limit != 5U,
        "the write is not the backup's LoRa section");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_plan_keeps_the_radios_own_keys, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    g_settings.security.serial_enabled = !g_settings.security.serial_enabled;
    const int planned = mesh_radio_backup_meshtastic_plan(
        &g_backup, &g_settings, &g_status, g_writes, MESH_RADIO_SETTINGS_TRANSACTION_MAX);
    MESH_TEST_FAIL_IF(planned != 1 || g_writes[0].kind != MESH_ADMIN_SET_CONFIG ||
                          g_writes[0].payload.config.which_payload_variant !=
                              meshtastic_Config_security_tag,
                      "the Security section was not the one write");
    const meshtastic_Config_SecurityConfig *security =
        &g_writes[0].payload.config.payload_variant.security;
    /* An empty private key would have the firmware make a new identity. */
    MESH_TEST_FAIL_IF(security->private_key.size != g_settings.security.private_key.size ||
                          security->private_key.size == 0U ||
                          memcmp(security->private_key.bytes, g_settings.security.private_key.bytes,
                                 security->private_key.size) != 0,
                      "the restore would have written the radio an empty private key");
    /* And with no key of the radio's own to carry, the section is not written at all. */
    g_settings.security.private_key.size = 0U;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_plan(&g_backup, &g_settings, &g_status, g_writes,
                                                        MESH_RADIO_SETTINGS_TRANSACTION_MAX) != 0,
                      "a Security write went out with no private key to put in it");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_plan_for_a_reset_radio_fits_one_transaction, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    /* The same radio after a factory reset: every section back to its defaults, still whole. */
    const meshtastic_Config_SecurityConfig keys = g_settings.security;
    struct mesh_radio_settings *reset = &g_restored;
    *reset = g_settings;
    memset(&reset->lora, 0, sizeof reset->lora);
    memset(&reset->device, 0, sizeof reset->device);
    memset(&reset->display, 0, sizeof reset->display);
    memset(&reset->position, 0, sizeof reset->position);
    memset(&reset->power, 0, sizeof reset->power);
    memset(&reset->bluetooth, 0, sizeof reset->bluetooth);
    memset(&reset->network, 0, sizeof reset->network);
    memset(&reset->security, 0, sizeof reset->security);
    reset->security.private_key = keys.private_key;
    reset->security.public_key = keys.public_key;
    for (size_t i = 0; i < MESH_RADIO_SETTINGS_MAX_CHANNELS; ++i) {
        memset(&reset->channels[i], 0, sizeof reset->channels[i]);
        reset->channels[i].index = (int8_t)i;
    }
    memset(&reset->owner.long_name, 0, sizeof reset->owner.long_name);
    const int planned = mesh_radio_backup_meshtastic_plan(&g_backup, reset, &g_status, g_writes,
                                                          MESH_RADIO_SETTINGS_TRANSACTION_MAX);
    /* The fixture's LoRa, device role, owner and its two named channels are what a reset
       loses; every other section it holds is at its defaults already. */
    MESH_TEST_FAIL_IF(planned != 5, "a reset radio was not planned the five sections it lost");

    static struct mesh_radio_settings queue;
    mesh_radio_settings_reset(&queue);
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_transaction(&queue, g_writes, (size_t)planned) !=
                          planned + 3,
                      "the restore did not fit one transaction");
    record_success(test_name);
}

MESH_TEST_CASE(radio_backup_meshtastic_plan_refuses_another_protocol, unit) {
    mesh_test_backup_radio(&g_settings, &g_status);
    mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    g_backup.header.protocol = MESH_RADIO_BACKUP_MESHCORE;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_plan(&g_backup, &g_settings, &g_status, g_writes,
                                                        4U) != -EPROTO,
                      "a MeshCore backup was planned onto a Meshtastic radio");
    record_success(test_name);
}
