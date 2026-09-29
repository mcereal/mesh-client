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
#include "inkwell/base/time.h"
#include "mesh/core/meshcore_backup.h"
#include "mesh/core/radio_backup_meshtastic.h"

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
