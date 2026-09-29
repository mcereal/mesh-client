/*
 * Profiles on the card, and the presses that make, compare, apply, export and import them.
 *
 * The profile itself - what goes in one, how it is compared, how it is planned, how a `.cfg` is
 * read and written - is core's (mesh/core/radio_profile.h and radio_profile_cfg.h). What is here
 * is where the directory is, which backup a new profile is being made from, which radio one is
 * compared with, and what each press says when it is done. Applying one is the restore's work,
 * and lives with it in app_backup.c.
 *
 * **The directory is `profiles/` beside the preferences file** - `~/.meshclient/profiles` on a
 * host - rather than beside the backups under a dotted name, because it is the one folder here
 * somebody is expected to open: a `.cfg` from a phone is copied into it, and one exported is
 * copied out of it.
 */

#include "app_internal.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"
#include "mesh/core/radio_profile.h"
#include "mesh/core/radio_profile_cfg.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/backups.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void app_profile_toast(struct mesh_app *app, const char *text) {
    mesh_ui_store_set_toast(&app->ui_store, inkwell_time_monotonic_ms(), text);
}

/* Which Settings section names each module, as a comparison's module change is named. */
static void app_profile_name_modules(struct mesh_ui_profiles *listing) {
    for (uint16_t tag = 0U; tag < 32U; ++tag) {
        uint16_t name = (uint16_t)(MESH_UI_BACKUPS_MODULE_UNPLACED + tag);
        for (uint32_t m = 0U; m < mesh_ui_settings_module_count(); ++m) {
            const enum mesh_ui_settings_section section = mesh_ui_settings_module_at(m);
            uint32_t type = 0U;
            const struct mesh_module_binding *binding = mesh_app_module_admin_type(section, &type)
                                                            ? mesh_radio_module_for_type(type)
                                                            : NULL;
            if (binding != NULL && binding->variant_tag == tag) {
                name = (uint16_t)section;
                break;
            }
        }
        listing->module_names[tag] = name;
    }
}

void mesh_app_profile_init(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }
    memset(&app->profiles, 0, sizeof app->profiles);
    app_profile_name_modules(&app->profile_listing);
    /* The preferences file's directory, and `profiles` in it. */
    char dir[sizeof app->ui_preferences_path + 16];
    inkwell_str_copy(dir, sizeof dir, app->ui_preferences_path);
    char *slash = strrchr(dir, '/');
    if (dir[0] == '\0' || slash == NULL) {
        inkwell_log_warn("app", "No preferences directory; not keeping profiles");
        return;
    }
    if (snprintf(slash + 1, sizeof dir - (size_t)(slash + 1 - dir), "profiles") >=
        (int)(sizeof dir - (size_t)(slash + 1 - dir))) {
        inkwell_log_warn("app", "Profile path truncated; not keeping profiles");
        return;
    }
    const int result = mesh_radio_profile_store_init(&app->profiles, dir);
    if (result < 0) {
        inkwell_log_warn("app", "Profiles unavailable: %d", result);
    }
    mesh_app_profile_rescan(app);
}

/* By name, as a person looks one up; a name only differing in case sorts by sequence. */
static int app_profile_by_name(const void *a, const void *b) {
    const struct mesh_ui_profile *left = a;
    const struct mesh_ui_profile *right = b;
    const int named = strcasecmp(left->header.name, right->header.name);
    if (named != 0) {
        return named;
    }
    return left->sequence < right->sequence ? -1 : left->sequence > right->sequence ? 1 : 0;
}

void mesh_app_profile_rescan(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }
    struct mesh_ui_profiles *listing = &app->profile_listing;
    listing->enabled = mesh_radio_profile_store_enabled(&app->profiles);
    listing->count = 0U;
    listing->cfg_count = 0U;
    /* The home directory as "~", which is the part of the path a person does not need read to
       them and the part most likely to push the rest off the row. */
    const char *home = getenv("HOME");
    const size_t home_len = home != NULL ? strlen(home) : 0U;
    if (home_len > 1U && strncmp(app->profiles.dir, home, home_len) == 0 &&
        app->profiles.dir[home_len] == '/') {
        snprintf(listing->folder, sizeof listing->folder, "~%s", app->profiles.dir + home_len);
    } else {
        inkwell_str_copy(listing->folder, sizeof listing->folder, app->profiles.dir);
    }
    if (!listing->enabled) {
        return;
    }
    /* More than are listed, so the ones listed are the newest when the card holds more. */
    uint32_t sequences[MESH_UI_PROFILES_MAX * 4U];
    const int total = mesh_radio_profile_store_list(&app->profiles, sequences,
                                                    sizeof sequences / sizeof *sequences);
    struct mesh_radio_backup *profile = malloc(sizeof *profile);
    if (total > 0 && profile != NULL) {
        const size_t kept = (size_t)total < sizeof sequences / sizeof *sequences
                                ? (size_t)total
                                : sizeof sequences / sizeof *sequences;
        for (size_t i = kept; i > 0U && listing->count < MESH_UI_PROFILES_MAX; --i) {
            /* One that does not read is not a profile, and the screen has nothing to say of it. */
            if (mesh_radio_profile_store_load(&app->profiles, sequences[i - 1U], profile) != 0) {
                continue;
            }
            struct mesh_ui_profile *item = &listing->items[listing->count++];
            item->sequence = sequences[i - 1U];
            item->header = profile->header;
        }
        qsort(listing->items, listing->count, sizeof listing->items[0], app_profile_by_name);
    }
    free(profile);
    const int cfgs =
        mesh_radio_profile_store_cfgs(&app->profiles, listing->cfgs, MESH_UI_PROFILE_CFGS_MAX);
    if (cfgs > 0) {
        listing->cfg_count =
            (uint8_t)((size_t)cfgs < MESH_UI_PROFILE_CFGS_MAX ? (size_t)cfgs
                                                              : MESH_UI_PROFILE_CFGS_MAX);
    }
}

void mesh_app_profile_publish(const struct mesh_app *app, struct mesh_ui_profiles *out) {
    if (app != NULL && out != NULL) {
        *out = app->profile_listing;
    }
}

void mesh_app_profile_compare(struct mesh_app *app, uint32_t sequence) {
    if (app == NULL) {
        return;
    }
    struct mesh_ui_profiles *listing = &app->profile_listing;
    listing->compare_sequence = sequence;
    listing->compare_state = MESH_UI_BACKUP_COMPARE_FAILED;
    listing->compare_error = 0;
    mesh_radio_backup_diff_reset(&listing->diff, MESH_RADIO_BACKUP_PROTOCOL_NONE);
    struct mesh_radio_backup *pair = malloc(2U * sizeof *pair);
    if (pair == NULL) {
        listing->compare_error = -ENOMEM;
        return;
    }
    struct mesh_radio_backup *profile = &pair[0];
    struct mesh_radio_backup *live = &pair[1];
    int result = mesh_radio_profile_store_load(&app->profiles, sequence, profile);
    if (result == 0) {
        result = mesh_app_backup_capture_live(app, live);
    }
    if (result == 0) {
        result = mesh_radio_profile_diff(profile, live, &listing->diff);
    }
    free(pair);
    if (result != 0) {
        inkwell_log_warn("app", "Comparing profile %u failed: %d", (unsigned)sequence, result);
        listing->compare_error = (int16_t)result;
        return;
    }
    /* A module is named by the section that edits it, as a backup's comparison is. */
    for (size_t i = 0; i < listing->diff.count; ++i) {
        struct mesh_radio_backup_change *change = &listing->diff.changes[i];
        if (change->topic == MESH_RADIO_BACKUP_TOPIC_MODULE && change->index < 32U) {
            change->index = listing->module_names[change->index];
        }
    }
    listing->compare_state = MESH_UI_BACKUP_COMPARE_DONE;
}

void mesh_app_profile_delete(struct mesh_app *app, uint32_t sequence) {
    if (app == NULL) {
        return;
    }
    char toast[MESH_UI_NAV_TOAST_MAX];
    const int result = mesh_radio_profile_store_remove(&app->profiles, sequence);
    if (result == 0) {
        inkwell_str_copy(toast, sizeof toast, inkcell_str(MESH_STR_TOAST_PROFILE_DELETED));
        inkwell_log_info("app", "Deleted profile %u", (unsigned)sequence);
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_PROFILE_DELETE_FAILED, result);
        inkwell_log_warn("app", "Deleting profile %u failed: %d", (unsigned)sequence, result);
    }
    app_profile_toast(app, toast);
    mesh_app_profile_rescan(app);
}

/* ---- making one out of a backup ------------------------------------------------------------ */

/* The backup a draft is of, read whole. */
static int app_profile_load_backup(struct mesh_app *app, uint32_t node, uint32_t sequence,
                                   struct mesh_radio_backup *backup) {
    struct mesh_radio_backup_entry entries[64];
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

void mesh_app_profile_draft(struct mesh_app *app, uint32_t node, uint32_t sequence) {
    if (app == NULL) {
        return;
    }
    struct mesh_ui_profiles *listing = &app->profile_listing;
    listing->draft_node = 0U;
    listing->draft_sequence = 0U;
    listing->draft_count = 0U;
    memset(&listing->draft_picked, 0, sizeof listing->draft_picked);
    struct mesh_radio_backup *backup = malloc(sizeof *backup);
    int result = backup == NULL ? -ENOMEM : app_profile_load_backup(app, node, sequence, backup);
    int offered = 0;
    if (result == 0) {
        offered =
            mesh_radio_profile_offer(backup, listing->draft_parts, MESH_RADIO_PROFILE_PARTS_MAX);
        result = offered < 0 ? offered : 0;
    }
    if (result == 0) {
        listing->draft_node = node;
        listing->draft_sequence = sequence;
        listing->draft_protocol = backup->header.protocol;
        listing->draft_count = (uint8_t)((size_t)offered < MESH_RADIO_PROFILE_PARTS_MAX
                                             ? (size_t)offered
                                             : MESH_RADIO_PROFILE_PARTS_MAX);
        /* Everything ticked: a profile is most often "this radio, for another one". */
        for (size_t i = 0; i < listing->draft_count; ++i) {
            mesh_radio_profile_parts_set(&listing->draft_picked, listing->draft_parts[i].topic,
                                         listing->draft_parts[i].index, true);
        }
    } else {
        char toast[MESH_UI_NAV_TOAST_MAX];
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_PROFILE_DRAFT_FAILED, result);
        app_profile_toast(app, toast);
        inkwell_log_warn("app", "Reading backup %u of 0x%08x for a profile failed: %d",
                         (unsigned)sequence, (unsigned)node, result);
    }
    free(backup);
}

void mesh_app_profile_draft_toggle(struct mesh_app *app, uint32_t index) {
    if (app == NULL || index >= app->profile_listing.draft_count) {
        return;
    }
    struct mesh_ui_profiles *listing = &app->profile_listing;
    const struct mesh_radio_profile_part *part = &listing->draft_parts[index];
    const bool on = mesh_radio_profile_parts_has(&listing->draft_picked, part->topic, part->index);
    mesh_radio_profile_parts_set(&listing->draft_picked, part->topic, part->index, !on);
}

void mesh_app_profile_make(struct mesh_app *app, const char *name) {
    if (app == NULL) {
        return;
    }
    struct mesh_ui_profiles *listing = &app->profile_listing;
    char toast[MESH_UI_NAV_TOAST_MAX];
    /* Without the spaces a keyboard leaves either side of a name. */
    const char *start = name != NULL ? name : "";
    while (*start == ' ') {
        ++start;
    }
    char trimmed[MESH_RADIO_BACKUP_TEXT];
    inkwell_str_copy(trimmed, sizeof trimmed, start);
    for (size_t end = strlen(trimmed); end > 0U && trimmed[end - 1U] == ' '; --end) {
        trimmed[end - 1U] = '\0';
    }
    if (listing->draft_node == 0U || trimmed[0] == '\0') {
        return;
    }
    if (listing->draft_picked.topics == 0U) {
        app_profile_toast(app, inkcell_str(MESH_STR_TOAST_PROFILE_NOTHING));
        return;
    }
    struct mesh_radio_backup *pair = malloc(2U * sizeof *pair);
    int result = pair == NULL ? -ENOMEM
                              : app_profile_load_backup(app, listing->draft_node,
                                                        listing->draft_sequence, &pair[0]);
    if (result == 0) {
        result = mesh_radio_profile_make(&pair[0], &listing->draft_picked, trimmed, &pair[1]);
    }
    uint32_t sequence = 0U;
    if (result == 0) {
        pair[1].header.saved_at = inkwell_time_wall_credible_s();
        result = mesh_radio_profile_store_save(&app->profiles, &pair[1], &sequence);
    }
    if (result == 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_PROFILE_SAVED, trimmed);
        inkwell_log_info("app", "Profile %u \"%s\" made from backup %u of 0x%08x: %zu sections",
                         (unsigned)sequence, trimmed, (unsigned)listing->draft_sequence,
                         (unsigned)listing->draft_node, pair[1].section_count);
        listing->draft_node = 0U;
        listing->draft_sequence = 0U;
        listing->draft_count = 0U;
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_PROFILE_SAVE_FAILED, result);
        inkwell_log_warn("app", "Making a profile failed: %d", result);
    }
    free(pair);
    app_profile_toast(app, toast);
    mesh_app_profile_rescan(app);
}

/* ---- .cfg ---------------------------------------------------------------------------------- */

/*
 * A profile's name as a file name: what a card's filesystem - FAT, as often as not - and the
 * phone it goes to next will both take. A character either would refuse is a '_', and a name
 * with nothing left is "profile".
 */
static void app_profile_file_name(const char *name, char *out, size_t out_len) {
    size_t used = 0U;
    for (const char *c = name; *c != '\0' && used + 5U < out_len; ++c) {
        const unsigned char byte = (unsigned char)*c;
        const bool refused = byte < 0x20U || strchr("/\\:*?\"<>|", *c) != NULL;
        if (used == 0U && (*c == '.' || *c == ' ')) {
            continue;
        }
        out[used++] = refused ? '_' : *c;
    }
    while (used > 0U && (out[used - 1U] == ' ' || out[used - 1U] == '.')) {
        --used;
    }
    out[used] = '\0';
    if (used == 0U) {
        inkwell_str_copy(out, out_len, "profile");
        used = strlen(out);
    }
    snprintf(out + used, out_len - used, ".cfg");
}

void mesh_app_profile_export(struct mesh_app *app, uint32_t sequence) {
    if (app == NULL) {
        return;
    }
    char toast[MESH_UI_NAV_TOAST_MAX];
    struct mesh_radio_backup *profile = malloc(sizeof *profile);
    int result = profile == NULL ? -ENOMEM
                                 : mesh_radio_profile_store_load(&app->profiles, sequence, profile);
    char file[MESH_RADIO_PROFILE_FILE_MAX] = "";
    if (result == 0) {
        app_profile_file_name(profile->header.name, file, sizeof file);
        char path[MESH_RADIO_BACKUP_PATH_MAX + MESH_RADIO_PROFILE_FILE_MAX];
        result = mesh_radio_profile_store_path(&app->profiles, file, path, sizeof path)
                     ? mesh_radio_profile_cfg_write(profile, path)
                     : -ENAMETOOLONG;
    }
    if (result == 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_PROFILE_EXPORTED, file);
        inkwell_log_info("app", "Profile %u written to %s", (unsigned)sequence, file);
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_PROFILE_EXPORT_FAILED, result);
        inkwell_log_warn("app", "Exporting profile %u failed: %d", (unsigned)sequence, result);
    }
    free(profile);
    app_profile_toast(app, toast);
    mesh_app_profile_rescan(app);
}

void mesh_app_profile_import(struct mesh_app *app, const char *file) {
    if (app == NULL || file == NULL) {
        return;
    }
    char toast[MESH_UI_NAV_TOAST_MAX];
    /* Named for its file, less the ".cfg". */
    char name[MESH_RADIO_BACKUP_TEXT];
    inkwell_str_copy(name, sizeof name, file);
    const size_t len = strlen(name);
    if (len > 4U && strcasecmp(name + len - 4U, ".cfg") == 0) {
        name[len - 4U] = '\0';
    }
    struct mesh_radio_backup *profile = malloc(sizeof *profile);
    char path[MESH_RADIO_BACKUP_PATH_MAX + MESH_RADIO_PROFILE_FILE_MAX];
    int result = profile == NULL ? -ENOMEM
                 : mesh_radio_profile_store_path(&app->profiles, file, path, sizeof path)
                     ? mesh_radio_profile_cfg_read(path, name, profile)
                     : -EINVAL;
    if (result == 0) {
        profile->header.saved_at = inkwell_time_wall_credible_s();
        result = mesh_radio_profile_store_save(&app->profiles, profile, NULL);
    }
    if (result == 0) {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_PROFILE_IMPORTED, name);
        inkwell_log_info("app", "Imported %s as a profile: %zu sections", file,
                         profile->section_count);
    } else {
        inkcell_str_format(toast, sizeof toast, MESH_STR_TOAST_PROFILE_IMPORT_FAILED, result);
        inkwell_log_warn("app", "Importing %s failed: %d", file, result);
    }
    free(profile);
    app_profile_toast(app, toast);
    mesh_app_profile_rescan(app);
}
