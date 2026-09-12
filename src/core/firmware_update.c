#define _POSIX_C_SOURCE 200809L

#include "mesh/core/firmware_update.h"

#include "mesh/i18n/strings.h"
#include "mesh/transport/ble_hci.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * How long READY waits for a radio to arm on.
 *
 * It is the BLE path's reconnect and nothing else: the download had the antenna, so the radio
 * has been unreachable for the length of it and auto-connect is starting over with its own
 * backoff. Thirty seconds is two of that backoff's early attempts plus a handshake, and the
 * failure it ends in is the mildest one there is - the image is staged, the radio is untouched,
 * and pressing again skips straight past the download because the release has not moved.
 */
#define MESH_FIRMWARE_UPDATE_READY_TIMEOUT_MS 30000U

static const char *firmware_update_staging(void) {
    const char *const from_env = getenv("MESHCLIENT_FIRMWARE_STAGING");
    return (from_env != NULL && from_env[0] != '\0') ? from_env
                                                     : MESH_FIRMWARE_UPDATE_STAGING_DEFAULT;
}

const char *mesh_firmware_update_state_name(enum mesh_firmware_update_state state) {
    switch (state) {
    case MESH_FIRMWARE_UPDATE_IDLE:
        return mesh_str(MESH_STR_FW_UPDATE_IDLE);
    case MESH_FIRMWARE_UPDATE_RESOLVING:
        return mesh_str(MESH_STR_FW_UPDATE_RESOLVING);
    case MESH_FIRMWARE_UPDATE_DOWNLOADING:
        return mesh_str(MESH_STR_FW_UPDATE_DOWNLOADING);
    case MESH_FIRMWARE_UPDATE_READY:
        return mesh_str(MESH_STR_FW_UPDATE_READY);
    case MESH_FIRMWARE_UPDATE_ARMING:
        return mesh_str(MESH_STR_FW_UPDATE_ARMING);
    case MESH_FIRMWARE_UPDATE_WAITING:
        return mesh_str(MESH_STR_FW_UPDATE_WAITING);
    case MESH_FIRMWARE_UPDATE_WRITING:
        return mesh_str(MESH_STR_FW_UPDATE_WRITING);
    case MESH_FIRMWARE_UPDATE_RESTARTING:
        return mesh_str(MESH_STR_FW_UPDATE_RESTARTING);
    case MESH_FIRMWARE_UPDATE_DONE:
        return mesh_str(MESH_STR_FW_UPDATE_DONE);
    case MESH_FIRMWARE_UPDATE_FAILED:
        return mesh_str(MESH_STR_FW_UPDATE_FAILED);
    case MESH_FIRMWARE_UPDATE_STATE_COUNT:
    default:
        return mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
    }
}

const char *mesh_firmware_update_error_name(enum mesh_firmware_update_error error) {
    switch (error) {
    case MESH_FIRMWARE_UPDATE_ERROR_UNAVAILABLE:
        return mesh_str(MESH_STR_FW_UPDATE_ERR_UNAVAILABLE);
    case MESH_FIRMWARE_UPDATE_ERROR_DOWNLOAD:
        return mesh_str(MESH_STR_FW_UPDATE_ERR_DOWNLOAD);
    case MESH_FIRMWARE_UPDATE_ERROR_WRONG_IMAGE:
        return mesh_str(MESH_STR_FW_UPDATE_ERR_WRONG_IMAGE);
    case MESH_FIRMWARE_UPDATE_ERROR_NO_RADIO:
        return mesh_str(MESH_STR_FW_UPDATE_ERR_NO_RADIO);
    case MESH_FIRMWARE_UPDATE_ERROR_REFUSED:
        return mesh_str(MESH_STR_FW_UPDATE_ERR_REFUSED);
    case MESH_FIRMWARE_UPDATE_ERROR_HANDOVER:
        return mesh_str(MESH_STR_FW_UPDATE_ERR_HANDOVER);
    case MESH_FIRMWARE_UPDATE_ERROR_NONE:
    case MESH_FIRMWARE_UPDATE_ERROR_COUNT:
    default:
        return "";
    }
}

/* One place that moves the ladder, so `revision` cannot be forgotten by a branch that set a
   state directly - the same trick firmware.c's firmware_set() plays next door. */
static void update_set(struct mesh_firmware_update *update, enum mesh_firmware_update_state state,
                       const char *detail) {
    if (update->state != state) {
        mesh_log_info("firmware-update", "%s -> %s", mesh_firmware_update_state_name(update->state),
                      mesh_firmware_update_state_name(state));
    }
    update->state = state;
    if (detail != NULL) {
        mesh_str_copy(update->detail, sizeof update->detail, detail);
    }
    update->revision++;
}

/*
 * Closes the BLE half down: the loader connection, then the D-Bus connection it was made on.
 *
 * Always in that order and always both, because the client is this job's own - nothing else in
 * the process holds it, so leaving it open leaks a socket the event loop is still watching.
 */
static void update_close_bluez(struct mesh_firmware_update *update) {
    if (!update->bluez_open) {
        return;
    }
    mesh_bluez_client_shutdown(&update->bluez);
    memset(&update->bluez, 0, sizeof update->bluez);
    update->bluez_open = false;
}

/* The radio has said everything it is going to; the app may take the link down. Once, because
   the hook is "stop using this bus" rather than "the state changed". */
static void update_release_link(struct mesh_firmware_update *update) {
    if (update->link_released) {
        return;
    }
    update->link_released = true;
    if (update->hooks.release_link != NULL) {
        update->hooks.release_link(update->hooks.userdata);
    }
}

static void update_finish(struct mesh_firmware_update *update,
                          enum mesh_firmware_update_error error, const char *detail) {
    update->error = error;
    update_set(update,
               error == MESH_FIRMWARE_UPDATE_ERROR_NONE ? MESH_FIRMWARE_UPDATE_DONE
                                                        : MESH_FIRMWARE_UPDATE_FAILED,
               detail);
    if (update->on_done != NULL) {
        update->on_done(update->userdata, update);
    }
}

/* ---- the handover, once the image is on disk ------------------------------------------------ */

/*
 * The USB install's states, folded onto the ladder.
 *
 * Written as a table rather than derived, for the reason src/ui/status.c writes its verbs out:
 * what the client says it is doing should be readable in one place, and a sub-module growing a
 * state should be a compile error here rather than a row that silently says the wrong thing.
 */
static enum mesh_firmware_update_state
update_state_of_install(enum mesh_firmware_install_state state) {
    switch (state) {
    case MESH_FIRMWARE_INSTALL_ARMING:
        return MESH_FIRMWARE_UPDATE_ARMING;
    case MESH_FIRMWARE_INSTALL_WAITING:
        return MESH_FIRMWARE_UPDATE_WAITING;
    case MESH_FIRMWARE_INSTALL_WRITING:
        return MESH_FIRMWARE_UPDATE_WRITING;
    case MESH_FIRMWARE_INSTALL_RESTARTING:
        return MESH_FIRMWARE_UPDATE_RESTARTING;
    case MESH_FIRMWARE_INSTALL_DONE:
        return MESH_FIRMWARE_UPDATE_DONE;
    case MESH_FIRMWARE_INSTALL_IDLE:
    case MESH_FIRMWARE_INSTALL_FAILED:
    case MESH_FIRMWARE_INSTALL_STATE_COUNT:
    default:
        return MESH_FIRMWARE_UPDATE_FAILED;
    }
}

static enum mesh_firmware_update_state update_state_of_ota(enum mesh_firmware_ota_state state) {
    switch (state) {
    case MESH_FIRMWARE_OTA_ARMING:
        return MESH_FIRMWARE_UPDATE_ARMING;
    /* Looking for the loader and getting a link to it are one sentence on a row: neither is
       something the reader can act on differently, and both are over in a couple of seconds. */
    case MESH_FIRMWARE_OTA_WAITING:
    case MESH_FIRMWARE_OTA_CONNECTING:
        return MESH_FIRMWARE_UPDATE_WAITING;
    case MESH_FIRMWARE_OTA_SENDING:
        return MESH_FIRMWARE_UPDATE_WRITING;
    case MESH_FIRMWARE_OTA_RESTARTING:
        return MESH_FIRMWARE_UPDATE_RESTARTING;
    case MESH_FIRMWARE_OTA_DONE:
        return MESH_FIRMWARE_UPDATE_DONE;
    case MESH_FIRMWARE_OTA_IDLE:
    case MESH_FIRMWARE_OTA_FAILED:
    case MESH_FIRMWARE_OTA_STATE_COUNT:
    default:
        return MESH_FIRMWARE_UPDATE_FAILED;
    }
}

/*
 * The install's twelve errors as the four a row asks about.
 *
 * WRONG_IMAGE and ARM keep their own answers because they are the two that mean the radio was
 * never touched, which is the distinction a reader acts on; everything from the bootloader
 * onwards is one handover that broke, and `detail` carries which.
 */
static enum mesh_firmware_update_error
update_error_of_install(enum mesh_firmware_install_error error) {
    switch (error) {
    case MESH_FIRMWARE_INSTALL_ERROR_UNAVAILABLE:
        return MESH_FIRMWARE_UPDATE_ERROR_UNAVAILABLE;
    case MESH_FIRMWARE_INSTALL_ERROR_WRONG_IMAGE:
        return MESH_FIRMWARE_UPDATE_ERROR_WRONG_IMAGE;
    case MESH_FIRMWARE_INSTALL_ERROR_ARM:
        return MESH_FIRMWARE_UPDATE_ERROR_NO_RADIO;
    default:
        return MESH_FIRMWARE_UPDATE_ERROR_HANDOVER;
    }
}

static enum mesh_firmware_update_error update_error_of_ota(enum mesh_firmware_ota_error error) {
    switch (error) {
    case MESH_FIRMWARE_OTA_ERROR_UNAVAILABLE:
        return MESH_FIRMWARE_UPDATE_ERROR_UNAVAILABLE;
    case MESH_FIRMWARE_OTA_ERROR_WRONG_IMAGE:
        return MESH_FIRMWARE_UPDATE_ERROR_WRONG_IMAGE;
    case MESH_FIRMWARE_OTA_ERROR_ARM:
        return MESH_FIRMWARE_UPDATE_ERROR_NO_RADIO;
    /* The radio's own refusal, which is the one failure with the radio still running and a
       sentence from the firmware to show for it. */
    case MESH_FIRMWARE_OTA_ERROR_REFUSED:
        return MESH_FIRMWARE_UPDATE_ERROR_REFUSED;
    default:
        return MESH_FIRMWARE_UPDATE_ERROR_HANDOVER;
    }
}

static void update_usb_done(void *userdata, const struct mesh_firmware_install *install) {
    struct mesh_firmware_update *const update = (struct mesh_firmware_update *)userdata;
    if (install->state == MESH_FIRMWARE_INSTALL_DONE) {
        update_finish(update, MESH_FIRMWARE_UPDATE_ERROR_NONE, update->release.version);
        return;
    }
    update_finish(update, update_error_of_install(install->error),
                  mesh_firmware_install_error_name(install->error));
}

static void update_ble_done(void *userdata, const struct mesh_firmware_ota *ota) {
    struct mesh_firmware_update *const update = (struct mesh_firmware_update *)userdata;
    /*
     * The connection goes back before anybody is told the job is over, which is the order that
     * matters rather than the tidiness: the completion below hands the adapter back to the
     * transport, and a client of ours still on the bus when that happens is a second reader on
     * a socket the transport is about to start popping messages off.
     *
     * Safe here because the OTA has already released the loader and every one of its finishing
     * paths calls this as its last statement - so nothing touches the client after we return.
     */
    update_close_bluez(update);
    if (ota->state == MESH_FIRMWARE_OTA_DONE) {
        update_finish(update, MESH_FIRMWARE_UPDATE_ERROR_NONE, update->release.version);
        return;
    }
    /* The radio's words or the loader's where there are any: a firmware that said why is more
       use than our name for the category it fell into. */
    update_finish(update, update_error_of_ota(ota->error),
                  ota->reason[0] != '\0' ? ota->reason : mesh_firmware_ota_error_name(ota->error));
}

static void update_begin_usb(struct mesh_firmware_update *update, const char *image_path) {
    const uint32_t family = mesh_uf2_family_for_architecture(update->board.architecture);
    const int started = mesh_firmware_install_start(
        &update->usb, update->loop, image_path, update->where, family, update->hooks.arm_usb,
        update->hooks.userdata, update_usb_done, update);
    if (started < 0) {
        update_finish(update, update_error_of_install(update->usb.error),
                      mesh_firmware_install_error_name(update->usb.error));
        return;
    }
    update_set(update, update_state_of_install(update->usb.state), NULL);
}

static void update_begin_ble(struct mesh_firmware_update *update, const char *image_path) {
    char adapter[MESH_FIRMWARE_OTA_PATH_MAX];
    int result = mesh_bluez_client_init_private(&update->bluez);
    if (result == 0) {
        update->bluez_open = true;
        (void)mesh_bluez_client_attach_loop(&update->bluez, update->loop);
        result = mesh_bluez_client_find_adapter(&update->bluez, adapter, sizeof adapter);
    }
    if (result < 0) {
        update_close_bluez(update);
        update_finish(update, MESH_FIRMWARE_UPDATE_ERROR_HANDOVER,
                      mesh_str(MESH_STR_FW_UPDATE_ERR_NO_ADAPTER));
        return;
    }

    struct mesh_firmware_ota_params params;
    memset(&params, 0, sizeof params);
    params.client = &update->bluez;
    params.adapter_path = adapter;
    params.image_path = image_path;
    params.architecture = update->board.architecture;
    params.radio_address = update->where;
    params.arm = update->hooks.arm_ble;
    params.arm_userdata = update->hooks.userdata;
    params.request_interval = update->hooks.request_interval;
    params.on_done = update_ble_done;
    params.userdata = update;

    if (mesh_firmware_ota_start(&update->ble, &params) < 0) {
        const enum mesh_firmware_update_error error = update_error_of_ota(update->ble.error);
        update_close_bluez(update);
        update_finish(update, error, mesh_firmware_ota_error_name(update->ble.error));
        return;
    }
    update_set(update, update_state_of_ota(update->ble.state), NULL);
}

/* The image has landed and been checked. From here the two buses part company for good. */
static void update_begin_handover(struct mesh_firmware_update *update) {
    char image_path[MESH_FETCH_PATH_MAX];
    if (mesh_firmware_fetch_image_path(&update->image, image_path, sizeof image_path) == NULL) {
        update_finish(update, MESH_FIRMWARE_UPDATE_ERROR_DOWNLOAD,
                      mesh_str(MESH_STR_FW_UPDATE_ERR_NO_IMAGE));
        return;
    }
    if (update->path == MESH_FIRMWARE_PATH_USB) {
        update_begin_usb(update, image_path);
    } else {
        update_begin_ble(update, image_path);
    }
}

/*
 * Whether the link has done its last job, which is **not** the moment the arm call returned.
 *
 * `mesh_session_radio_action()` and `mesh_session_request_ble_ota()` *queue*; the admin queue
 * drains from the session's tick, which a transport calls from its own, so the request reaches
 * the radio some turns of the loop later. A link dropped when the arm returned would take the
 * wire out from under the packet that is the whole point of the press - which is exactly the
 * queue-here-drain-there split every other admin verb in this client lives with.
 *
 * So the signal is the handover leaving ARMING. On the BLE path that is the radio's own
 * "Rebooting to BLE OTA" or the arming clock running out; on the USB path it is the port going
 * away. Either way the radio has said everything it is going to.
 */
static void update_release_link_when_armed(struct mesh_firmware_update *update) {
    if (update->state == MESH_FIRMWARE_UPDATE_ARMING) {
        return;
    }
    update_release_link(update);
}

/* ---- the download ---------------------------------------------------------------------------- */

static enum mesh_firmware_update_error update_error_of_fetch(enum mesh_firmware_fetch_error error) {
    switch (error) {
    case MESH_FIRMWARE_FETCH_ERROR_UNAVAILABLE:
        return MESH_FIRMWARE_UPDATE_ERROR_UNAVAILABLE;
    /* Not a transfer failure: the bytes arrived and are for something else. The one guard
       between this board and an image for another one with the same architecture. */
    case MESH_FIRMWARE_FETCH_ERROR_WRONG_IMAGE:
    case MESH_FIRMWARE_FETCH_ERROR_MISMATCH:
        return MESH_FIRMWARE_UPDATE_ERROR_WRONG_IMAGE;
    default:
        return MESH_FIRMWARE_UPDATE_ERROR_DOWNLOAD;
    }
}

static void update_image_done(void *userdata, const struct mesh_firmware_fetch *fetch) {
    struct mesh_firmware_update *const update = (struct mesh_firmware_update *)userdata;
    if (fetch->state != MESH_FIRMWARE_FETCH_READY) {
        update_finish(update, update_error_of_fetch(fetch->error), fetch->message);
        return;
    }
    /*
     * The image is here and the antenna is free again. On the USB path the radio never went
     * away and the handover starts on this turn; on the BLE path it did, so this is where the
     * waiting starts and mesh_firmware_update_holds_the_antenna() stops being true - which is
     * what lets auto-connect bring the radio back to be armed.
     */
    update_set(update, MESH_FIRMWARE_UPDATE_READY, fetch->image.name);
    update->deadline_ms = 0U;
}

/* ---- lifecycle --------------------------------------------------------------------------------
 */

int mesh_firmware_update_init(struct mesh_firmware_update *update, struct mesh_event_loop *loop) {
    if (update == NULL) {
        return -EINVAL;
    }
    memset(update, 0, sizeof *update);
    update->loop = loop;
    update->state = MESH_FIRMWARE_UPDATE_IDLE;
    return mesh_fetch_init(&update->fetch, loop);
}

void mesh_firmware_update_shutdown(struct mesh_firmware_update *update) {
    if (update == NULL) {
        return;
    }
    mesh_firmware_update_cancel(update);
    mesh_fetch_shutdown(&update->fetch);
}

void mesh_firmware_update_use_ca_bundle(struct mesh_firmware_update *update, const char *path) {
    if (update == NULL) {
        return;
    }
    mesh_fetch_set_ca_bundle(&update->fetch, path);
}

bool mesh_firmware_update_available(const struct mesh_firmware_update *update) {
    return update != NULL && mesh_fetch_available(&update->fetch);
}

int mesh_firmware_update_start(struct mesh_firmware_update *update,
                               const struct mesh_firmware_board *board,
                               const struct mesh_firmware_release *release, const char *where,
                               const struct mesh_firmware_update_hooks *hooks,
                               mesh_firmware_update_done_fn on_done, void *userdata) {
    if (update == NULL || board == NULL || release == NULL || hooks == NULL) {
        return -EINVAL;
    }
    if (mesh_firmware_update_busy(update)) {
        return -EBUSY;
    }
    update->error = MESH_FIRMWARE_UPDATE_ERROR_NONE;
    update->detail[0] = '\0';
    update->link_released = false;

    /*
     * Everything that makes this impossible, answered before anything is started.
     *
     * All three are already rows in Settings - the blocker table is phase 1's whole job - so
     * reaching one of them here means a press was offered that should not have been. It is
     * still checked, because the press and the row are two readings of the same state and a
     * module that trusted its caller about which board this is would be the one place a wrong
     * image could get through.
     */
    if (!mesh_firmware_update_available(update) || release->version[0] == '\0' ||
        release->manifest_url[0] == '\0' || board->target[0] == '\0' ||
        board->path == MESH_FIRMWARE_PATH_NONE) {
        update->error = MESH_FIRMWARE_UPDATE_ERROR_UNAVAILABLE;
        update_set(update, MESH_FIRMWARE_UPDATE_FAILED,
                   mesh_str(MESH_STR_FW_UPDATE_ERR_UNAVAILABLE));
        return -ENOTSUP;
    }

    update->board = *board;
    update->path = board->path;
    update->hw_model = board->hw_model;
    update->release = *release;
    mesh_str_copy(update->where, sizeof update->where, where != NULL ? where : "");
    mesh_str_copy(update->staging, sizeof update->staging, firmware_update_staging());
    update->hooks = *hooks;
    update->on_done = on_done;
    update->userdata = userdata;

    const int started = mesh_firmware_fetch_start(
        &update->image, &update->fetch, board->target, release->version, release->manifest_url,
        board->architecture, update->staging, update_image_done, update);
    if (started < 0) {
        update->error = update_error_of_fetch(update->image.error);
        update_set(update, MESH_FIRMWARE_UPDATE_FAILED, update->image.message);
        return started;
    }
    mesh_log_info("firmware-update", "Installing %s %s over %s", board->target, release->version,
                  update->path == MESH_FIRMWARE_PATH_USB ? "USB" : "BLE");
    update_set(update, MESH_FIRMWARE_UPDATE_RESOLVING, release->version);
    return 0;
}

void mesh_firmware_update_radio_said(struct mesh_firmware_update *update, const char *text) {
    if (update == NULL || update->path != MESH_FIRMWARE_PATH_BLE) {
        return;
    }
    mesh_firmware_ota_radio_said(&update->ble, text);
}

void mesh_firmware_update_cancel(struct mesh_firmware_update *update) {
    if (update == NULL) {
        return;
    }
    mesh_firmware_fetch_cancel(&update->image);
    mesh_firmware_install_cancel(&update->usb);
    mesh_firmware_ota_cancel(&update->ble);
    update_close_bluez(update);
    if (mesh_firmware_update_busy(update)) {
        update->error = MESH_FIRMWARE_UPDATE_ERROR_HANDOVER;
        update_set(update, MESH_FIRMWARE_UPDATE_FAILED, mesh_str(MESH_STR_FW_UPDATE_ERR_CANCELLED));
    }
}

void mesh_firmware_update_tick(struct mesh_firmware_update *update, uint64_t now_ms) {
    if (update == NULL) {
        return;
    }
    mesh_fetch_tick(&update->fetch, now_ms);

    switch (update->state) {
    case MESH_FIRMWARE_UPDATE_RESOLVING:
    case MESH_FIRMWARE_UPDATE_DOWNLOADING: {
        mesh_firmware_fetch_tick(&update->image, now_ms);
        /* The two documents and the image are three steps of one fetch and only the last has a
           bar; the ladder says which of the two sentences is true right now. */
        if (mesh_firmware_update_busy(update)) {
            const enum mesh_firmware_update_state next =
                update->image.state == MESH_FIRMWARE_FETCH_DOWNLOADING
                    ? MESH_FIRMWARE_UPDATE_DOWNLOADING
                    : MESH_FIRMWARE_UPDATE_RESOLVING;
            if (next != update->state) {
                update_set(update, next, NULL);
            }
        }
        break;
    }
    case MESH_FIRMWARE_UPDATE_READY: {
        /*
         * Waiting for a radio to arm on, which on the USB path is already true and on the BLE
         * path is auto-connect finishing what the download interrupted. The deadline is armed
         * on the first tick here rather than when the state was entered, because the state is
         * entered from a fetch completion that has no clock to hand - the same reason
         * firmware.c keeps the caller's.
         */
        if (update->deadline_ms == 0U) {
            update->deadline_ms = now_ms + MESH_FIRMWARE_UPDATE_READY_TIMEOUT_MS;
        }
        const bool ready =
            update->hooks.radio_ready == NULL || update->hooks.radio_ready(update->hooks.userdata);
        if (ready) {
            update_begin_handover(update);
        } else if (now_ms >= update->deadline_ms) {
            update_finish(update, MESH_FIRMWARE_UPDATE_ERROR_NO_RADIO,
                          mesh_str(MESH_STR_FW_UPDATE_ERR_NO_RADIO));
        }
        break;
    }
    case MESH_FIRMWARE_UPDATE_ARMING:
    case MESH_FIRMWARE_UPDATE_WAITING:
    case MESH_FIRMWARE_UPDATE_WRITING:
    case MESH_FIRMWARE_UPDATE_RESTARTING: {
        if (update->path == MESH_FIRMWARE_PATH_USB) {
            mesh_firmware_install_tick(&update->usb, now_ms);
            if (mesh_firmware_install_busy(&update->usb)) {
                update_set(update, update_state_of_install(update->usb.state), NULL);
            }
            update_release_link_when_armed(update);
            break;
        }
        /* The loader's answers arrive on this client's own D-Bus connection, so somebody has to
           pop them; nothing else in the process is reading that socket. */
        if (update->bluez_open) {
            (void)mesh_bluez_client_process(&update->bluez);
        }
        mesh_firmware_ota_tick(&update->ble, now_ms);
        if (mesh_firmware_ota_busy(&update->ble)) {
            update_set(update, update_state_of_ota(update->ble.state), NULL);
        }
        update_release_link_when_armed(update);
        break;
    }
    case MESH_FIRMWARE_UPDATE_IDLE:
    case MESH_FIRMWARE_UPDATE_DONE:
    case MESH_FIRMWARE_UPDATE_FAILED:
    case MESH_FIRMWARE_UPDATE_STATE_COUNT:
    default:
        break;
    }
}

bool mesh_firmware_update_state_busy(enum mesh_firmware_update_state state) {
    switch (state) {
    case MESH_FIRMWARE_UPDATE_RESOLVING:
    case MESH_FIRMWARE_UPDATE_DOWNLOADING:
    case MESH_FIRMWARE_UPDATE_READY:
    case MESH_FIRMWARE_UPDATE_ARMING:
    case MESH_FIRMWARE_UPDATE_WAITING:
    case MESH_FIRMWARE_UPDATE_WRITING:
    case MESH_FIRMWARE_UPDATE_RESTARTING:
        return true;
    default:
        return false;
    }
}

bool mesh_firmware_update_busy(const struct mesh_firmware_update *update) {
    return update != NULL && mesh_firmware_update_state_busy(update->state);
}

bool mesh_firmware_update_holds_the_antenna(const struct mesh_firmware_update *update) {
    if (update == NULL) {
        return false;
    }
    return update->state == MESH_FIRMWARE_UPDATE_RESOLVING ||
           update->state == MESH_FIRMWARE_UPDATE_DOWNLOADING;
}

bool mesh_firmware_update_holds_the_radio(const struct mesh_firmware_update *update) {
    if (update == NULL) {
        return false;
    }
    return mesh_firmware_install_holds_the_radio(&update->usb) ||
           mesh_firmware_ota_holds_the_radio(&update->ble);
}

bool mesh_firmware_update_radio_in_loader(const struct mesh_firmware_update *update) {
    return update != NULL && mesh_firmware_ota_radio_in_loader(&update->ble);
}

bool mesh_firmware_update_can_resume(const struct mesh_firmware_update *update) {
    return mesh_firmware_update_radio_in_loader(update) && !mesh_firmware_update_busy(update) &&
           update->board.target[0] != '\0' && update->release.manifest_url[0] != '\0';
}

unsigned mesh_firmware_update_progress(const struct mesh_firmware_update *update) {
    if (update == NULL) {
        return 0U;
    }
    switch (update->state) {
    case MESH_FIRMWARE_UPDATE_DOWNLOADING:
        return mesh_firmware_fetch_progress(&update->image);
    case MESH_FIRMWARE_UPDATE_WRITING:
        return update->path == MESH_FIRMWARE_PATH_USB ? mesh_firmware_install_progress(&update->usb)
                                                      : mesh_firmware_ota_progress(&update->ble);
    default:
        return 0U;
    }
}
