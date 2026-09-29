/*
 * When a radio's settings are copied to the card, and from which conversation.
 *
 * The container, the store and both captures are core's (mesh/core/radio_backup.h and the two
 * beside it); what is here is the part that has to know which protocol holds the link and what
 * the client is about to do to the radio. Four moments take a backup on their own - the first
 * time a radio is read, and just before a save, an import, a factory reset or a firmware install
 * - and a fifth is somebody asking for one.
 *
 * **An automatic backup that would change nothing is not written.** Every save takes one, and a
 * run of saves that each change one row would otherwise fill the radio's directory with copies
 * of the same radio and push the one worth having out of the prune. So the newest backup on the
 * card is read and compared, sections to sections; a manual press is never skipped.
 *
 * The backup is about a hundred kilobytes and taken a few times an hour at most, so it is
 * allocated for the call rather than carried in struct mesh_app for the life of the process.
 */

#include "app_internal.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"
#include "mesh/core/meshcore_backup.h"
#include "mesh/core/radio_backup_meshtastic.h"
#include "mesh/ui/backups.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int app_backup_capture(struct mesh_app *app, struct mesh_radio_backup *backup) {
    if (app->meshcore_bound) {
        return mesh_meshcore_backup_capture(&app->meshcore, backup);
    }
    return mesh_radio_backup_meshtastic_capture(mesh_session_settings(&app->session),
                                                mesh_session_handshake(&app->session), backup);
}

/* Whether the newest backup of this radio already holds exactly these sections. */
static bool app_backup_unchanged(const struct mesh_app *app,
                                 const struct mesh_radio_backup *backup) {
    struct mesh_radio_backup_entry newest;
    if (mesh_radio_backup_store_list(&app->backups, backup->header.node_id, &newest, 1U) <= 0) {
        return false;
    }
    struct mesh_radio_backup *previous = malloc(sizeof *previous);
    if (previous == NULL) {
        return false;
    }
    const bool same = mesh_radio_backup_store_load(&app->backups, backup->header.node_id, &newest,
                                                   previous) == 0 &&
                      previous->header.protocol == backup->header.protocol &&
                      mesh_radio_backup_same_payload(previous, backup);
    free(previous);
    return same;
}

int mesh_app_backup_take(struct mesh_app *app, uint8_t reason) {
    if (app == NULL || mesh_radio_backup_reason_key(reason) == NULL) {
        return -EINVAL;
    }
    if (!mesh_radio_backup_store_enabled(&app->backups)) {
        return -ENODEV;
    }
    struct mesh_radio_backup *backup = malloc(sizeof *backup);
    if (backup == NULL) {
        return -ENOMEM;
    }
    int result = app_backup_capture(app, backup);
    if (result == 0) {
        backup->header.reason = reason;
        backup->header.saved_at = inkwell_time_wall_credible_s();
        const char *device = mesh_app_connected_identifier();
        snprintf(backup->header.device, sizeof backup->header.device, "%s",
                 device != NULL ? device : "");
        if (reason != MESH_RADIO_BACKUP_MANUAL && app_backup_unchanged(app, backup)) {
            inkwell_log_debug("app", "Backup of 0x%08x before %s skipped: unchanged",
                              (unsigned)backup->header.node_id,
                              mesh_radio_backup_reason_key(reason));
            free(backup);
            return 0;
        }
        struct mesh_radio_backup_entry entry;
        result = mesh_radio_backup_store_save(&app->backups, backup, &entry);
        if (result == 0) {
            inkwell_log_info("app", "Backed up 0x%08x (%s): %s, %zu sections",
                             (unsigned)backup->header.node_id, mesh_radio_backup_reason_key(reason),
                             entry.file, backup->section_count);
            result = 1;
        }
    }
    free(backup);
    if (result > 0) {
        mesh_app_backup_rescan(app);
    }
    return result;
}

void mesh_app_backup_before(struct mesh_app *app, uint8_t reason) {
    const int result = mesh_app_backup_take(app, reason);
    if (result < 0 && result != -ENODEV && result != -EAGAIN) {
        inkwell_log_warn("app", "Backup before %s failed: %d", mesh_radio_backup_reason_key(reason),
                         result);
    }
}

/* The radio on the link once it is ready to be backed up, or 0. */
static uint32_t app_backup_ready_node(struct mesh_app *app) {
    if (app->meshcore_bound) {
        return mesh_meshcore_backup_ready(&app->meshcore) ? app->meshcore.self_node : 0U;
    }
    const struct mesh_radio_settings *settings = mesh_session_settings(&app->session);
    const struct mesh_handshake_status *status = mesh_session_handshake(&app->session);
    /* And not while the late extras - the canned messages, the ringtone - are still arriving,
       which is the one part of a Meshtastic radio the handshake does not bring. */
    if (status == NULL || !status->config_complete || !status->has_my_info ||
        !mesh_radio_backup_meshtastic_ready(settings) || mesh_radio_settings_busy(settings)) {
        return 0U;
    }
    return status->my_info.my_node_num;
}

void mesh_app_backup_tick(struct mesh_app *app) {
    if (app == NULL || !mesh_radio_backup_store_enabled(&app->backups)) {
        return;
    }
    const uint32_t node = app_backup_ready_node(app);
    if (node == 0U || node == app->backup_checked_node) {
        return;
    }
    app->backup_checked_node = node;
    if (mesh_radio_backup_store_list(&app->backups, node, NULL, 0U) == 0) {
        mesh_app_backup_before(app, MESH_RADIO_BACKUP_FIRST_CONNECT);
    }
}

/* ---- the Backups section -------------------------------------------------------------------- */

/* The most backups of one radio the section lists: ten automatic ones and a couple asked for. */
#define APP_BACKUP_PER_RADIO 12U

struct app_backup_radio {
    uint32_t node;
    int total;
    size_t count;
    struct mesh_radio_backup_entry entries[APP_BACKUP_PER_RADIO];
    struct mesh_radio_backup_header newest;
};

/* Newest first by the time the newest backup was taken; a radio with no clock sorts last. */
static int app_backup_newest_first(const void *a, const void *b) {
    const uint32_t left = ((const struct app_backup_radio *)a)->newest.saved_at;
    const uint32_t right = ((const struct app_backup_radio *)b)->newest.saved_at;
    return left < right ? 1 : left > right ? -1 : 0;
}

void mesh_app_backup_rescan(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }
    struct mesh_ui_backups *listing = &app->backup_listing;
    listing->enabled = mesh_radio_backup_store_enabled(&app->backups);
    listing->radio_count = 0U;
    listing->entry_count = 0U;
    if (!listing->enabled) {
        return;
    }
    uint32_t nodes[MESH_UI_BACKUP_RADIOS_MAX];
    const int found =
        mesh_radio_backup_store_radios(&app->backups, nodes, MESH_UI_BACKUP_RADIOS_MAX);
    if (found <= 0) {
        return;
    }
    struct mesh_radio_backup *backup = malloc(sizeof *backup);
    struct app_backup_radio *radios = calloc(MESH_UI_BACKUP_RADIOS_MAX, sizeof *radios);
    if (backup == NULL || radios == NULL) {
        free(backup);
        free(radios);
        return;
    }
    /* Each radio's newest backup that reads, which names it and orders the list. */
    size_t kept = 0U;
    const size_t listed =
        (size_t)found < MESH_UI_BACKUP_RADIOS_MAX ? (size_t)found : MESH_UI_BACKUP_RADIOS_MAX;
    for (size_t i = 0; i < listed; ++i) {
        struct app_backup_radio *radio = &radios[kept];
        memset(radio, 0, sizeof *radio);
        radio->node = nodes[i];
        radio->total = mesh_radio_backup_store_list(&app->backups, nodes[i], radio->entries,
                                                    APP_BACKUP_PER_RADIO);
        if (radio->total <= 0) {
            continue;
        }
        radio->count = (size_t)radio->total < APP_BACKUP_PER_RADIO ? (size_t)radio->total
                                                                   : APP_BACKUP_PER_RADIO;
        for (size_t e = 0; e < radio->count; ++e) {
            if (mesh_radio_backup_store_load(&app->backups, nodes[i], &radio->entries[e], backup) ==
                0) {
                radio->newest = backup->header;
                ++kept;
                break;
            }
        }
    }
    if (kept > 1U) {
        qsort(radios, kept, sizeof *radios, app_backup_newest_first);
    }
    /* And every backup's header, a radio's together. One that does not read is left out: a
       file cut short is not a backup, and the section has nothing to say about it. */
    for (size_t r = 0; r < kept; ++r) {
        struct mesh_ui_backup_radio *radio = &listing->radios[listing->radio_count];
        memset(radio, 0, sizeof *radio);
        radio->node = radios[r].node;
        radio->count = (uint16_t)radios[r].total;
        radio->protocol = radios[r].newest.protocol;
        inkwell_str_copy(radio->name, sizeof radio->name, radios[r].newest.name);
        inkwell_str_copy(radio->device, sizeof radio->device, radios[r].newest.device);
        for (size_t e = 0; e < radios[r].count && listing->entry_count < MESH_UI_BACKUP_ENTRIES_MAX;
             ++e) {
            if (mesh_radio_backup_store_load(&app->backups, radios[r].node, &radios[r].entries[e],
                                             backup) != 0) {
                continue;
            }
            struct mesh_ui_backup_entry *entry = &listing->entries[listing->entry_count++];
            entry->radio = listing->radio_count;
            entry->sequence = radios[r].entries[e].sequence;
            entry->header = backup->header;
            ++radio->listed;
        }
        ++listing->radio_count;
    }
    free(radios);
    free(backup);
}

void mesh_app_backup_publish(struct mesh_app *app, struct mesh_ui_backups *out) {
    if (app == NULL || out == NULL) {
        return;
    }
    *out = app->backup_listing;
    out->live_node = 0U;
    out->live_protocol = MESH_RADIO_BACKUP_PROTOCOL_NONE;
    if (mesh_radio_backup_store_enabled(&app->backups)) {
        out->live_node = app_backup_ready_node(app);
        if (out->live_node != 0U) {
            out->live_protocol =
                app->meshcore_bound ? MESH_RADIO_BACKUP_MESHCORE : MESH_RADIO_BACKUP_MESHTASTIC;
        }
    }
}

/* A backup on the card by its node and sequence, read whole. */
static int app_backup_load(struct mesh_app *app, uint32_t node, uint32_t sequence,
                           struct mesh_radio_backup *backup) {
    struct mesh_radio_backup_entry entries[APP_BACKUP_PER_RADIO * 4U];
    const int total = mesh_radio_backup_store_list(&app->backups, node, entries,
                                                   sizeof entries / sizeof entries[0]);
    if (total < 0) {
        return total;
    }
    const size_t count = (size_t)total < sizeof entries / sizeof entries[0]
                             ? (size_t)total
                             : sizeof entries / sizeof entries[0];
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].sequence == sequence) {
            return mesh_radio_backup_store_load(&app->backups, node, &entries[i], backup);
        }
    }
    return -ENOENT;
}

/* A Meshtastic module's number, as the comparison names it, is its ModuleConfig variant tag;
   the screen names the Settings section that edits it instead. */
static void app_backup_name_modules(struct mesh_radio_backup_diff *diff) {
    for (size_t i = 0; i < diff->count; ++i) {
        struct mesh_radio_backup_change *change = &diff->changes[i];
        if (change->topic != MESH_RADIO_BACKUP_TOPIC_MODULE) {
            continue;
        }
        uint16_t section = (uint16_t)MESH_UI_SETTINGS_SECTION_COUNT;
        for (uint32_t m = 0U; m < mesh_ui_settings_module_count(); ++m) {
            const enum mesh_ui_settings_section candidate = mesh_ui_settings_module_at(m);
            uint32_t type = 0U;
            const struct mesh_module_binding *binding = mesh_app_module_admin_type(candidate, &type)
                                                            ? mesh_radio_module_for_type(type)
                                                            : NULL;
            if (binding != NULL && binding->variant_tag == change->index) {
                section = (uint16_t)candidate;
                break;
            }
        }
        change->index = section;
    }
}

void mesh_app_backup_compare(struct mesh_app *app, uint32_t node, uint32_t sequence) {
    if (app == NULL) {
        return;
    }
    struct mesh_ui_backups *listing = &app->backup_listing;
    listing->compare_node = node;
    listing->compare_sequence = sequence;
    listing->compare_state = MESH_UI_BACKUP_COMPARE_FAILED;
    listing->compare_error = 0;
    mesh_radio_backup_diff_reset(&listing->diff, MESH_RADIO_BACKUP_PROTOCOL_NONE);

    struct mesh_radio_backup *pair = malloc(2U * sizeof *pair);
    if (pair == NULL) {
        listing->compare_error = -ENOMEM;
        return;
    }
    struct mesh_radio_backup *saved = &pair[0];
    struct mesh_radio_backup *live = &pair[1];
    int result = app_backup_ready_node(app) == node ? 0 : -ENODEV;
    if (result == 0) {
        result = app_backup_load(app, node, sequence, saved);
    }
    if (result == 0) {
        result = app_backup_capture(app, live);
    }
    if (result == 0 && saved->header.protocol != live->header.protocol) {
        result = -EPROTO;
    }
    if (result == 0) {
        result = saved->header.protocol == MESH_RADIO_BACKUP_MESHCORE
                     ? mesh_meshcore_backup_diff(saved, live, &listing->diff)
                     : mesh_radio_backup_meshtastic_diff(saved, live, &listing->diff);
    }
    free(pair);
    if (result != 0) {
        inkwell_log_warn("app", "Comparing backup %u of 0x%08x failed: %d", (unsigned)sequence,
                         (unsigned)node, result);
        listing->compare_error = (int16_t)result;
        return;
    }
    app_backup_name_modules(&listing->diff);
    listing->compare_state = MESH_UI_BACKUP_COMPARE_DONE;
}

void mesh_app_backup_delete(struct mesh_app *app, uint32_t node, uint32_t sequence) {
    if (app == NULL) {
        return;
    }
    char toast[MESH_UI_NAV_TOAST_MAX];
    const int result = mesh_radio_backup_store_remove(&app->backups, node, sequence);
    if (result == 0) {
        inkwell_str_copy(toast, sizeof toast, inkcell_str(MESH_STR_TOAST_BACKUP_DELETED));
        inkwell_log_info("app", "Deleted backup %u of 0x%08x", (unsigned)sequence, (unsigned)node);
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_BACKUP_DELETE_FAILED, result);
        inkwell_log_warn("app", "Deleting backup %u of 0x%08x failed: %d", (unsigned)sequence,
                         (unsigned)node, result);
    }
    mesh_ui_store_set_toast(&app->ui_store, inkwell_time_monotonic_ms(), toast);
    mesh_app_backup_rescan(app);
}
