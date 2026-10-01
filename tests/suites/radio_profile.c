#define _POSIX_C_SOURCE 200809L

/*
 * Profiles: a radio's settings with the radio taken out, the directory they are kept in, and
 * Meshtastic's `.cfg`.
 *
 * What the rest rests on is the first case: a profile made from a backup carries none of what
 * makes a radio itself, whatever was ticked. Then that a profile touches only its parts - on the
 * screen that compares it, and in the writes that apply it - that a `.cfg` goes out and comes
 * back the same, and that a profile of one protocol is never put on a radio of the other.
 * MeshCore's half of the same claims is with the rest of MeshCore, in meshcore.c.
 */

#include "framework/mesh_test.h"
#include "support/backup_fixture.h"
#include "support/fs_fixture.h"

#include "inkwell/base/file.h"
#include "mesh/core/radio_backup.h"
#include "mesh/core/radio_backup_meshtastic.h"
#include "mesh/core/radio_profile.h"
#include "mesh/core/radio_profile_cfg.h"
#include "mesh/proto/channel_url.h"
#include "meshtastic/clientonly.pb.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct mesh_radio_settings g_settings;
static struct mesh_handshake_status g_status;
static struct mesh_radio_backup g_backup;
static struct mesh_radio_backup g_profile;
static struct mesh_radio_backup g_read;
static struct mesh_radio_backup g_live;
static struct mesh_radio_backup_diff g_diff;
static struct mesh_admin_request g_writes[MESH_RADIO_SETTINGS_TRANSACTION_MAX];
static uint8_t g_cfg[MESH_RADIO_PROFILE_CFG_MAX];

static bool profile_tempdir(char *dir, size_t dir_len) {
    snprintf(dir, dir_len, "/tmp/mesh_profile_XXXXXX");
    return mkdtemp(dir) != NULL;
}

/* The fixture radio, with a fixed position so there is one to leave out, captured. */
static void profile_capture(void) {
    mesh_test_backup_radio(&g_settings, &g_status);
    g_settings.position.fixed_position = true;
    g_status.node_count = 1U;
    g_status.nodes[0].node_id = 0x0badcafeU;
    g_status.nodes[0].position.valid = true;
    g_status.nodes[0].position.latitude_i = 473977000;
    g_status.nodes[0].position.longitude_i = 85456000;
    (void)mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    g_backup.header.reason = MESH_RADIO_BACKUP_MANUAL;
    g_backup.header.saved_at = 1767225600U;
    snprintf(g_backup.header.device, sizeof g_backup.header.device, "/dev/ttyUSB0");
}

static struct mesh_radio_backup_parts profile_every(void) {
    return (struct mesh_radio_backup_parts){.topics = UINT32_MAX, .modules = UINT32_MAX};
}

static struct mesh_radio_backup_parts profile_of(uint8_t topic) {
    struct mesh_radio_backup_parts parts = {0};
    mesh_radio_profile_parts_set(&parts, topic, 0U, true);
    return parts;
}

static size_t profile_count_topic(const struct mesh_radio_backup_diff *diff, uint8_t topic) {
    size_t count = 0U;
    for (size_t i = 0; i < diff->count; ++i) {
        count += diff->changes[i].topic == topic ? 1U : 0U;
    }
    return count;
}

/* ---- making one ---------------------------------------------------------------------------- */

MESH_TEST_CASE(radio_profile_from_a_backup_has_no_identity, unit) {
    profile_capture();
    MESH_TEST_FAIL_IF(mesh_radio_backup_count_tag(&g_backup, MESH_RADIO_BACKUP_MT_OWNER) != 1U ||
                          mesh_radio_backup_count_tag(&g_backup, MESH_RADIO_BACKUP_MT_POSITION) !=
                              1U,
                      "fixture: the backup has no owner or fixed position to leave out");
    /* Everything ticked - which is not a way to put the radio back in. */
    const struct mesh_radio_backup_parts every = profile_every();
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &every, "Ridge kit", &g_profile) != 0,
                      "the profile was not made");

    const struct mesh_radio_backup_header *header = &g_profile.header;
    MESH_TEST_FAIL_IF(header->reason != MESH_RADIO_BACKUP_PROFILE || header->node_id != 0U ||
                          header->device[0] != '\0' || strcmp(header->name, "Ridge kit") != 0 ||
                          header->has_nodes_heard,
                      "the header still names the radio it came from");
    MESH_TEST_FAIL_IF(mesh_radio_backup_count_tag(&g_profile, MESH_RADIO_BACKUP_MT_OWNER) != 0U,
                      "the owner went into a profile");
    MESH_TEST_FAIL_IF(mesh_radio_backup_count_tag(&g_profile, MESH_RADIO_BACKUP_MT_POSITION) != 0U,
                      "the fixed position went into a profile");
    MESH_TEST_FAIL_IF(
        mesh_radio_profile_parts_has(&header->parts, MESH_RADIO_BACKUP_TOPIC_SECURITY, 0U) ||
            mesh_radio_profile_parts_has(&header->parts, MESH_RADIO_BACKUP_TOPIC_OWNER, 0U),
        "the header claims an identity part");

    /* Read back as settings: no Security section, so no key of any kind, and the position
       settings no longer say the radio is pinned to a place. */
    static struct mesh_radio_settings read;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_read(&g_profile, &read, NULL) != 0,
                      "the profile's sections did not decode");
    MESH_TEST_FAIL_IF(read.has_security || read.has_owner, "a key or the owner is in the profile");
    MESH_TEST_FAIL_IF(!read.has_position || read.position.fixed_position,
                      "the profile would pin every radio it is put on to this one's place");
    MESH_TEST_FAIL_IF(!read.has_lora || !read.has_channel[2] || !read.has_telemetry ||
                          !read.has_canned_messages,
                      "a part that is not identity was left out");
    record_success(test_name);
}

/* Nor from one that carries the radio's private key: every part ticked still leaves it behind,
   and a profile that somehow had one would not be written. */
MESH_TEST_CASE(radio_profile_from_a_keyed_backup_has_no_key, unit) {
    profile_capture();
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_add_identity(&g_settings, &g_backup) != 0,
                      "fixture: the key was not added");
    const struct mesh_radio_backup_parts every = profile_every();
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &every, "Ridge kit", &g_profile) != 0,
                      "the profile was not made");
    MESH_TEST_FAIL_IF(g_profile.header.has_identity ||
                          mesh_radio_backup_identity(&g_profile, NULL) != 0U,
                      "the private key went into a profile");
    bool key_bytes = false;
    const uint8_t *data = NULL;
    const struct mesh_radio_backup_section *section = NULL;
    const uint8_t run[8] = {0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB};
    for (size_t i = 0; (section = mesh_radio_backup_section_at(&g_profile, i, &data)) != NULL;
         ++i) {
        for (size_t at = 0; at + sizeof run <= section->len; ++at) {
            key_bytes = key_bytes || memcmp(data + at, run, sizeof run) == 0;
        }
    }
    MESH_TEST_FAIL_IF(key_bytes, "the private key's bytes are in the profile");

    /* And into a .cfg, which is where a profile goes to leave this client. */
    const int len = mesh_radio_profile_cfg_encode(&g_profile, g_cfg, sizeof g_cfg);
    bool cfg_key = false;
    for (int at = 0; len > 0 && at + (int)sizeof run <= len; ++at) {
        cfg_key = cfg_key || memcmp(g_cfg + at, run, sizeof run) == 0;
    }
    MESH_TEST_FAIL_IF(len <= 0 || cfg_key, "the private key went into a .cfg");
    record_success(test_name);
}

MESH_TEST_CASE(radio_profile_keeps_only_the_parts_picked, unit) {
    profile_capture();
    struct mesh_radio_profile_part offer[MESH_RADIO_PROFILE_PARTS_MAX];
    const int offered = mesh_radio_profile_offer(&g_backup, offer, MESH_RADIO_PROFILE_PARTS_MAX);
    bool lora = false;
    bool channels = false;
    bool identity = false;
    for (int i = 0; i < offered; ++i) {
        lora = lora || offer[i].topic == MESH_RADIO_BACKUP_TOPIC_LORA;
        channels = channels || offer[i].topic == MESH_RADIO_BACKUP_TOPIC_CHANNEL;
        identity = identity ||
                   !mesh_radio_profile_part_allowed(MESH_RADIO_BACKUP_MESHTASTIC, offer[i].topic);
    }
    MESH_TEST_FAIL_IF(!lora || !channels, "LoRa or the channels were not offered");
    MESH_TEST_FAIL_IF(identity, "an identity part was offered");

    struct mesh_radio_backup_parts parts = profile_of(MESH_RADIO_BACKUP_TOPIC_LORA);
    mesh_radio_profile_parts_set(&parts, MESH_RADIO_BACKUP_TOPIC_CHANNEL, 0U, true);
    mesh_radio_profile_parts_set(&parts, MESH_RADIO_BACKUP_TOPIC_MODULE,
                                 meshtastic_ModuleConfig_telemetry_tag, true);
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &parts, "Mesh", &g_profile) != 0,
                      "the profile was not made");
    MESH_TEST_FAIL_IF(mesh_radio_backup_count_tag(&g_profile, MESH_RADIO_BACKUP_MT_CONFIG) != 1U ||
                          mesh_radio_backup_count_tag(&g_profile, MESH_RADIO_BACKUP_MT_CHANNEL) !=
                              MESH_RADIO_BACKUP_CHANNELS ||
                          mesh_radio_backup_count_tag(&g_profile, MESH_RADIO_BACKUP_MT_MODULE) !=
                              1U ||
                          g_profile.section_count != 2U + MESH_RADIO_BACKUP_CHANNELS,
                      "not one LoRa section, the whole channel table and one module");
    MESH_TEST_FAIL_IF(!g_profile.header.has_radio || g_profile.header.channel_count != 3U,
                      "the header lost the LoRa numbers or the channel names it carries");

    /* Nothing that is a part is not a profile. */
    const struct mesh_radio_backup_parts owner = profile_of(MESH_RADIO_BACKUP_TOPIC_OWNER);
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &owner, "Me", &g_profile) != -EINVAL,
                      "a profile of the owner alone was made");
    record_success(test_name);
}

/* ---- comparing and applying ---------------------------------------------------------------- */

/*
 * Compared with a radio, a profile of LoRa is about LoRa: every section it leaves out is left
 * out of the comparison too, rather than listed as "only on the radio" - and a radio that
 * differs from it only outside its parts matches it.
 */
MESH_TEST_CASE(radio_profile_compares_only_its_parts, unit) {
    profile_capture();
    const struct mesh_radio_backup_parts lora = profile_of(MESH_RADIO_BACKUP_TOPIC_LORA);
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &lora, "LoRa", &g_profile) != 0,
                      "the profile was not made");

    /* Another radio: another owner, another role, somewhere else. */
    g_settings.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    snprintf(g_settings.owner.long_name, sizeof g_settings.owner.long_name, "Valley");
    g_settings.position.fixed_position = false;
    g_status.my_info.my_node_num = 0x12345678U;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_live) != 0,
                      "capture failed");
    MESH_TEST_FAIL_IF(mesh_radio_profile_diff(&g_profile, &g_live, &g_diff) != 0,
                      "the comparison failed");
    MESH_TEST_FAIL_IF(g_diff.total != 0U, "a difference outside the profile's parts was listed");

    g_settings.lora.hop_limit = 2U;
    (void)mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_live);
    MESH_TEST_FAIL_IF(mesh_radio_profile_diff(&g_profile, &g_live, &g_diff) != 0 ||
                          g_diff.total != 1U ||
                          profile_count_topic(&g_diff, MESH_RADIO_BACKUP_TOPIC_LORA) != 1U,
                      "the one LoRa difference was not the one listed");
    record_success(test_name);
}

MESH_TEST_CASE(radio_profile_applied_writes_only_its_sections, unit) {
    profile_capture();
    struct mesh_radio_backup_parts parts = profile_of(MESH_RADIO_BACKUP_TOPIC_LORA);
    mesh_radio_profile_parts_set(&parts, MESH_RADIO_BACKUP_TOPIC_CHANNEL, 0U, true);
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &parts, "Mesh", &g_profile) != 0,
                      "the profile was not made");

    /* A radio that differs everywhere: LoRa, a channel, its role, its name, its canned list. */
    g_settings.lora.hop_limit = 2U;
    snprintf(g_settings.channels[2].settings.name, sizeof g_settings.channels[2].settings.name,
             "Other");
    g_settings.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    snprintf(g_settings.owner.long_name, sizeof g_settings.owner.long_name, "Valley");
    snprintf(g_settings.canned_messages, sizeof g_settings.canned_messages, "Hi");
    size_t unwritable = 99U;
    const int planned = mesh_radio_backup_meshtastic_plan(&g_profile, &g_settings, &g_status,
                                                          g_writes, 8U, &unwritable);
    MESH_TEST_FAIL_IF(planned != 2 || unwritable != 0U,
                      "not exactly the LoRa write and the one channel's");
    bool lora = false;
    bool channel = false;
    for (int i = 0; i < planned; ++i) {
        lora = lora ||
               (g_writes[i].kind == MESH_ADMIN_SET_CONFIG &&
                g_writes[i].payload.config.which_payload_variant == meshtastic_Config_lora_tag &&
                g_writes[i].payload.config.payload_variant.lora.hop_limit == 5U);
        channel = channel || (g_writes[i].kind == MESH_ADMIN_SET_CHANNEL &&
                              g_writes[i].payload.channel.index == 2 &&
                              strcmp(g_writes[i].payload.channel.settings.name, "Ops") == 0);
    }
    MESH_TEST_FAIL_IF(!lora || !channel, "the writes are not the profile's LoRa and slot 2");
    record_success(test_name);
}

/*
 * A radio pinned to a place stays pinned, and one that is not stays free: the profile's position
 * settings go on with the radio's own flag in them, and the flag alone is no difference.
 */
MESH_TEST_CASE(radio_profile_applied_keeps_the_radios_own_fixed_position, unit) {
    profile_capture();
    const struct mesh_radio_backup_parts position = profile_of(MESH_RADIO_BACKUP_TOPIC_POSITION);
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &position, "Pos", &g_profile) != 0,
                      "the profile was not made");
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_plan(&g_profile, &g_settings, &g_status,
                                                        g_writes, 8U, NULL) != 0,
                      "the fixed flag alone was a write");

    g_settings.position.position_broadcast_secs = 60U;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_plan(&g_profile, &g_settings, &g_status,
                                                        g_writes, 8U, NULL) != 1,
                      "a changed position setting was not one write");
    const meshtastic_Config_PositionConfig *written =
        &g_writes[0].payload.config.payload_variant.position;
    MESH_TEST_FAIL_IF(!written->fixed_position,
                      "the write would have freed a radio that was pinned to its place");
    MESH_TEST_FAIL_IF(written->position_broadcast_secs == 60U,
                      "the write is not the profile's position settings");
    record_success(test_name);
}

MESH_TEST_CASE(radio_profile_is_refused_by_a_radio_of_the_other_protocol, unit) {
    profile_capture();
    const struct mesh_radio_backup_parts lora = profile_of(MESH_RADIO_BACKUP_TOPIC_LORA);
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &lora, "LoRa", &g_profile) != 0,
                      "the profile was not made");
    g_live = g_backup;
    g_live.header.protocol = MESH_RADIO_BACKUP_MESHCORE;
    MESH_TEST_FAIL_IF(mesh_radio_profile_diff(&g_profile, &g_live, &g_diff) != -EPROTO,
                      "a Meshtastic profile was compared with a MeshCore radio");
    g_profile.header.protocol = MESH_RADIO_BACKUP_MESHCORE;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_plan(&g_profile, &g_settings, &g_status,
                                                        g_writes, 8U, NULL) != -EPROTO,
                      "a MeshCore profile was planned onto a Meshtastic radio");
    record_success(test_name);
}

/* ---- the directory ------------------------------------------------------------------------- */

MESH_TEST_CASE(radio_profile_store_round_trips_and_lists_cfgs, unit) {
    profile_capture();
    struct mesh_radio_backup_parts parts = profile_of(MESH_RADIO_BACKUP_TOPIC_LORA);
    mesh_radio_profile_parts_set(&parts, MESH_RADIO_BACKUP_TOPIC_MODULE,
                                 meshtastic_ModuleConfig_telemetry_tag, true);
    (void)mesh_radio_profile_make(&g_backup, &parts, "Base=camp", &g_profile);

    char dir[64];
    MESH_TEST_FAIL_IF(!profile_tempdir(dir, sizeof dir), "mkdtemp failed");
    static struct mesh_radio_profile_store store;
    MESH_TEST_FAIL_IF(mesh_radio_profile_store_init(&store, dir) != 0, "the store did not open");
    uint32_t first = 0U;
    uint32_t second = 0U;
    const int saved = mesh_radio_profile_store_save(&store, &g_profile, &first) |
                      mesh_radio_profile_store_save(&store, &g_profile, &second);
    /* A backup is not a profile, even in the profile directory. */
    const int backup = mesh_radio_profile_store_save(&store, &g_backup, NULL);
    char path[160];
    FILE *cfg = mesh_radio_profile_store_path(&store, "Phone.CFG", path, sizeof path)
                    ? fopen(path, "wb")
                    : NULL;
    if (cfg != NULL) {
        fputc(0, cfg);
        fclose(cfg);
    }
    uint32_t listed[4];
    const int count = mesh_radio_profile_store_list(&store, listed, 4U);
    char cfgs[4][MESH_RADIO_PROFILE_FILE_MAX];
    const int cfg_count = mesh_radio_profile_store_cfgs(&store, cfgs, 4U);
    const int loaded = mesh_radio_profile_store_load(&store, second, &g_read);
    /* Capped, the list keeps the newest: the one just saved is never the one left out. */
    uint32_t capped[1];
    const int capped_total = mesh_radio_profile_store_list(&store, capped, 1U);
    const int removed = mesh_radio_profile_store_remove(&store, first);
    const int after = mesh_radio_profile_store_list(&store, listed, 4U);
    const bool escape = mesh_radio_profile_store_path(&store, "../x.cfg", path, sizeof path) ||
                        mesh_radio_profile_store_path(&store, ".hidden", path, sizeof path);
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(saved != 0 || first != 1U || second != 2U, "two saves were not 1 and 2");
    MESH_TEST_FAIL_IF(backup != -EINVAL, "a backup was saved as a profile");
    MESH_TEST_FAIL_IF(count != 2, "the store did not list both profiles");
    MESH_TEST_FAIL_IF(capped_total != 2 || capped[0] != 2U,
                      "a list cut to one kept an older profile than the newest");
    MESH_TEST_FAIL_IF(cfg_count != 1 || strcmp(cfgs[0], "Phone.CFG") != 0,
                      "the .cfg beside them was not listed");
    MESH_TEST_FAIL_IF(loaded != 0 || !mesh_radio_backup_same_payload(&g_read, &g_profile) ||
                          strcmp(g_read.header.name, "Base=camp") != 0 ||
                          g_read.header.parts.topics != g_profile.header.parts.topics ||
                          g_read.header.parts.modules != g_profile.header.parts.modules,
                      "the profile did not read back with its name and parts");
    MESH_TEST_FAIL_IF(removed != 0 || after != 1 || listed[0] != 2U,
                      "the remove took the wrong one");
    MESH_TEST_FAIL_IF(escape, "a name off a screen reached outside the directory");
    record_success(test_name);
}

/* ---- .cfg ---------------------------------------------------------------------------------- */

MESH_TEST_CASE(radio_profile_cfg_round_trips_through_device_profile, unit) {
    profile_capture();
    /* A channel link is compact - a primary, then the secondaries, no holes - so a secondary
       after an empty slot comes back one slot up, as it does through the apps' own export. The
       fixture's "Ops" is moved next to the primary to be a table a link can carry as it is. */
    g_settings.channels[1] = g_settings.channels[2];
    g_settings.channels[1].index = 1;
    memset(&g_settings.channels[2], 0, sizeof g_settings.channels[2]);
    g_settings.channels[2].index = 2;
    g_settings.channels[2].has_settings = true;
    (void)mesh_radio_backup_meshtastic_capture(&g_settings, &g_status, &g_backup);
    const struct mesh_radio_backup_parts every = profile_every();
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &every, "Ridge kit", &g_profile) != 0,
                      "the profile was not made");
    const int len = mesh_radio_profile_cfg_encode(&g_profile, g_cfg, sizeof g_cfg);
    MESH_TEST_FAIL_IF(len <= 0, "the profile did not encode");
    MESH_TEST_FAIL_IF(mesh_radio_profile_cfg_decode(g_cfg, (size_t)len, "Imported", &g_read) != 0,
                      "the file did not decode");
    MESH_TEST_FAIL_IF(strcmp(g_read.header.name, "Imported") != 0 ||
                          g_read.header.reason != MESH_RADIO_BACKUP_PROFILE,
                      "the import is not a profile named for its file");

    /* The same radio, part for part: compared with the profile it came from, the import is
       the same in every part a DeviceProfile has room for. Radio UI has none, and Position is
       left out: its cleared fixed_position would read as false to the apps, and unpin a radio. */
    struct mesh_radio_backup_parts cfg_parts = g_profile.header.parts;
    mesh_radio_profile_parts_set(&cfg_parts, MESH_RADIO_BACKUP_TOPIC_RADIO_UI, 0U, false);
    mesh_radio_profile_parts_set(&cfg_parts, MESH_RADIO_BACKUP_TOPIC_POSITION, 0U, false);
    MESH_TEST_FAIL_IF(!mesh_radio_profile_parts_has(&g_profile.header.parts,
                                                    MESH_RADIO_BACKUP_TOPIC_POSITION, 0U),
                      "fixture: the profile has no Position part to leave out");
    meshtastic_DeviceProfile *raw = calloc(1U, sizeof *raw);
    pb_istream_t stream = pb_istream_from_buffer(g_cfg, (size_t)len);
    const bool raw_read = raw != NULL && pb_decode(&stream, meshtastic_DeviceProfile_fields, raw);
    const bool raw_position = raw_read && raw->config.has_position;
    free(raw);
    MESH_TEST_FAIL_IF(!raw_read, "the file is not a DeviceProfile");
    MESH_TEST_FAIL_IF(raw_position, "the .cfg carries a Position section");
    MESH_TEST_FAIL_IF(g_read.header.parts.topics != cfg_parts.topics ||
                          g_read.header.parts.modules != cfg_parts.modules,
                      "the import does not carry the parts the profile did");
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_diff(&g_profile, &g_read, &g_diff) != 0,
                      "the two did not compare");
    for (size_t i = 0; i < g_diff.count; ++i) {
        MESH_TEST_FAIL_IF(g_diff.changes[i].topic != MESH_RADIO_BACKUP_TOPIC_RADIO_UI &&
                              g_diff.changes[i].topic != MESH_RADIO_BACKUP_TOPIC_POSITION,
                          "a part changed on its way through a .cfg");
    }
    MESH_TEST_FAIL_IF(g_read.header.channel_count != 2U ||
                          strcmp(g_read.header.channel_names[1], "Ops") != 0 ||
                          strcmp(g_read.header.region, g_profile.header.region) != 0,
                      "the import's header does not say what is in it");

    /* And once more: out of the import, the same bytes. */
    static uint8_t again[MESH_RADIO_PROFILE_CFG_MAX];
    const int len_again = mesh_radio_profile_cfg_encode(&g_read, again, sizeof again);
    MESH_TEST_FAIL_IF(len_again != len || memcmp(again, g_cfg, (size_t)len) != 0,
                      "a second trip through a .cfg changed it");
    record_success(test_name);
}

/* What another app writes into a DeviceProfile that a profile never carries: the owner's names,
   a fixed position, the keys. An import leaves every one of them behind. */
MESH_TEST_CASE(radio_profile_cfg_import_leaves_the_radio_behind, unit) {
    static meshtastic_DeviceProfile device;
    memset(&device, 0, sizeof device);
    device.has_long_name = true;
    snprintf(device.long_name, sizeof device.long_name, "Somebody");
    device.has_short_name = true;
    snprintf(device.short_name, sizeof device.short_name, "SB");
    device.has_fixed_position = true;
    device.fixed_position.has_latitude_i = true;
    device.fixed_position.latitude_i = 473977000;
    device.has_config = true;
    device.config.has_lora = true;
    device.config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    device.config.lora.use_preset = true;
    device.config.lora.hop_limit = 4U;
    device.config.has_security = true;
    device.config.security.private_key.size = 32U;
    memset(device.config.security.private_key.bytes, 0xAB, 32U);
    device.has_module_config = true;
    device.module_config.has_mqtt = true;
    device.module_config.mqtt.enabled = true;
    snprintf(device.module_config.mqtt.address, sizeof device.module_config.mqtt.address,
             "mqtt.example.org");
    pb_ostream_t stream = pb_ostream_from_buffer(g_cfg, sizeof g_cfg);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_DeviceProfile_fields, &device),
                      "fixture: the DeviceProfile did not encode");

    MESH_TEST_FAIL_IF(
        mesh_radio_profile_cfg_decode(g_cfg, stream.bytes_written, "phone", &g_profile) != 0,
        "the file did not import");
    MESH_TEST_FAIL_IF(g_profile.section_count != 2U,
                      "not the LoRa config and the MQTT module, and nothing else");
    bool key_bytes = false;
    const uint8_t run[8] = {0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB};
    const uint8_t *data = NULL;
    const struct mesh_radio_backup_section *section = NULL;
    for (size_t i = 0; (section = mesh_radio_backup_section_at(&g_profile, i, &data)) != NULL;
         ++i) {
        for (size_t at = 0; at + sizeof run <= section->len; ++at) {
            key_bytes = key_bytes || memcmp(data + at, run, sizeof run) == 0;
        }
    }
    MESH_TEST_FAIL_IF(key_bytes, "the file's private key was imported");
    MESH_TEST_FAIL_IF(!mesh_radio_profile_parts_has(&g_profile.header.parts,
                                                    MESH_RADIO_BACKUP_TOPIC_MODULE,
                                                    meshtastic_ModuleConfig_mqtt_tag) ||
                          strcmp(g_profile.header.region, "US") != 0,
                      "the MQTT module or the region did not come in");

    /* And a file with nothing a profile carries is not one. */
    memset(&device, 0, sizeof device);
    device.has_long_name = true;
    snprintf(device.long_name, sizeof device.long_name, "Somebody");
    stream = pb_ostream_from_buffer(g_cfg, sizeof g_cfg);
    (void)pb_encode(&stream, meshtastic_DeviceProfile_fields, &device);
    MESH_TEST_FAIL_IF(
        mesh_radio_profile_cfg_decode(g_cfg, stream.bytes_written, "names", &g_profile) != -EINVAL,
        "a file of names alone was made a profile");
    const uint8_t junk[] = {0xff, 0xff, 0xff};
    MESH_TEST_FAIL_IF(mesh_radio_profile_cfg_decode(junk, sizeof junk, "junk", &g_profile) !=
                          -EBADMSG,
                      "bytes that are not a DeviceProfile were imported");
    record_success(test_name);
}

/* The channel link a .cfg carries is the table whole: an import of one channel switches the
   other seven off, as the apps' own import does. */
MESH_TEST_CASE(radio_profile_cfg_channel_link_is_the_whole_table, unit) {
    static meshtastic_DeviceProfile device;
    memset(&device, 0, sizeof device);
    meshtastic_ChannelSet set = meshtastic_ChannelSet_init_zero;
    set.settings_count = 1U;
    snprintf(set.settings[0].name, sizeof set.settings[0].name, "Hikers");
    set.settings[0].psk.size = 1U;
    set.settings[0].psk.bytes[0] = 1U;
    static char url[MESH_CHANNEL_URL_MAX];
    MESH_TEST_FAIL_IF(mesh_channel_url_encode(&set, false, url, sizeof url) == 0U,
                      "fixture: the link did not encode");
    /* Encoded by hand: a DeviceProfile's link is a callback, and this is somebody else's file. */
    uint8_t *at = g_cfg;
    const size_t url_len = strlen(url);
    *at++ = (3U << 3U) | 2U;
    size_t n = url_len;
    while (n >= 0x80U) {
        *at++ = (uint8_t)(n | 0x80U);
        n >>= 7U;
    }
    *at++ = (uint8_t)n;
    memcpy(at, url, url_len);
    at += url_len;

    MESH_TEST_FAIL_IF(
        mesh_radio_profile_cfg_decode(g_cfg, (size_t)(at - g_cfg), "hike", &g_profile) != 0,
        "the file did not import");
    static struct mesh_radio_settings read;
    MESH_TEST_FAIL_IF(mesh_radio_backup_meshtastic_read(&g_profile, &read, NULL) != 0,
                      "the profile did not decode");
    MESH_TEST_FAIL_IF(read.channels[0].role != meshtastic_Channel_Role_PRIMARY ||
                          strcmp(read.channels[0].settings.name, "Hikers") != 0,
                      "the link's channel is not the primary");
    for (size_t i = 1; i < MESH_MESHTASTIC_CHANNELS; ++i) {
        MESH_TEST_FAIL_IF(!read.has_channel[i] ||
                              read.channels[i].role != meshtastic_Channel_Role_DISABLED,
                          "a slot past the link's channels was not switched off");
    }
    record_success(test_name);
}

/* Position alone - the one part an export leaves out - is no file at all, not an empty one. */
MESH_TEST_CASE(radio_profile_cfg_of_nothing_it_carries_is_refused, unit) {
    profile_capture();
    const struct mesh_radio_backup_parts position = profile_of(MESH_RADIO_BACKUP_TOPIC_POSITION);
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &position, "Pos", &g_profile) != 0,
                      "the profile was not made");
    MESH_TEST_FAIL_IF(mesh_radio_profile_cfg_encode(&g_profile, g_cfg, sizeof g_cfg) != -ENODATA,
                      "a profile of nothing a .cfg carries encoded");
    record_success(test_name);
}

MESH_TEST_CASE(radio_profile_cfg_file_round_trips, unit) {
    profile_capture();
    const struct mesh_radio_backup_parts lora = profile_of(MESH_RADIO_BACKUP_TOPIC_LORA);
    (void)mesh_radio_profile_make(&g_backup, &lora, "LoRa", &g_profile);
    char dir[64];
    MESH_TEST_FAIL_IF(!profile_tempdir(dir, sizeof dir), "mkdtemp failed");
    char path[128];
    snprintf(path, sizeof path, "%s/LoRa.cfg", dir);
    const int written = mesh_radio_profile_cfg_write(&g_profile, path);
    const int read = mesh_radio_profile_cfg_read(path, "LoRa", &g_read);
    char temp[160];
    snprintf(temp, sizeof temp, "%s.tmp", path);
    const bool temp_left = access(temp, F_OK) == 0;
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(written != 0 || read != 0, "the file did not round-trip");
    MESH_TEST_FAIL_IF(temp_left, "the temporary was left beside the file");
    MESH_TEST_FAIL_IF(!mesh_radio_backup_same_payload(&g_read, &g_profile),
                      "the profile read back is not the one written");
    record_success(test_name);
}

/* Created only where nothing is: a file already under the name is refused and left as it was. */
MESH_TEST_CASE(radio_profile_cfg_create_never_replaces_a_file, unit) {
    profile_capture();
    const struct mesh_radio_backup_parts lora = profile_of(MESH_RADIO_BACKUP_TOPIC_LORA);
    (void)mesh_radio_profile_make(&g_backup, &lora, "LoRa", &g_profile);
    char dir[64];
    MESH_TEST_FAIL_IF(!profile_tempdir(dir, sizeof dir), "mkdtemp failed");
    char path[128];
    snprintf(path, sizeof path, "%s/LoRa.cfg", dir);
    const int created = mesh_radio_profile_cfg_create(&g_profile, path);
    const int read = mesh_radio_profile_cfg_read(path, "LoRa", &g_read);
    char other[128];
    snprintf(other, sizeof other, "%s/phone.cfg", dir);
    FILE *file = fopen(other, "wb");
    if (file != NULL) {
        fputs("keys", file);
        fclose(file);
    }
    const int again = mesh_radio_profile_cfg_create(&g_profile, other);
    size_t len = 0U;
    uint8_t *kept = inkwell_file_read(other, 64U, &len);
    const bool untouched = kept != NULL && len == 4U && memcmp(kept, "keys", 4U) == 0;
    free(kept);
    mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(created != 0 || read != 0, "a new file was not created and read back");
    MESH_TEST_FAIL_IF(again != -EEXIST, "a file already there was not refused");
    MESH_TEST_FAIL_IF(!untouched, "a file already there was written over");
    record_success(test_name);
}
