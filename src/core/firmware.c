#define _POSIX_C_SOURCE 200809L

#include "mesh/core/firmware.h"

#include "mesh/i18n/strings.h"

#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The two documents, and how much of each we are willing to read.
 *
 * Both are overridable so a test can point them at a script on PATH, the same way
 * MESHCLIENT_UPDATE_REPO does for the client's own release check.
 *
 * The caps are generous because these documents are bigger than they sound. The hardware list
 * is 39 KB for 116 boards and grows by a board at a time; the release index is **155 KB**,
 * almost all of it release notes written by whoever merged the pull request. Neither is a
 * reply we could ask to be smaller - there is no per-board endpoint and no way to opt out of
 * the notes - so the honest answer is a cap with room in it, held for the seconds the check
 * takes and then freed.
 */
#ifndef MESHCLIENT_FIRMWARE_HARDWARE_URL
#define MESHCLIENT_FIRMWARE_HARDWARE_URL "https://api.meshtastic.org/resource/deviceHardware"
#endif
#ifndef MESHCLIENT_FIRMWARE_LIST_URL
#define MESHCLIENT_FIRMWARE_LIST_URL "https://api.meshtastic.org/github/firmware/list"
#endif

#define MESH_FIRMWARE_HARDWARE_MAX (256U * 1024U)
#define MESH_FIRMWARE_LIST_MAX (512U * 1024U)
#define MESH_FIRMWARE_TIMEOUT_MS 30000U

static const char *firmware_hardware_url(void) {
    const char *const from_env = getenv("MESHCLIENT_FIRMWARE_HARDWARE_URL");
    return (from_env != NULL && from_env[0] != '\0') ? from_env : MESHCLIENT_FIRMWARE_HARDWARE_URL;
}

static const char *firmware_list_url(void) {
    const char *const from_env = getenv("MESHCLIENT_FIRMWARE_LIST_URL");
    return (from_env != NULL && from_env[0] != '\0') ? from_env : MESHCLIENT_FIRMWARE_LIST_URL;
}

const char *mesh_firmware_state_name(enum mesh_firmware_state state) {
    switch (state) {
    case MESH_FIRMWARE_IDLE:
        return mesh_str(MESH_STR_FW_STATE_IDLE);
    case MESH_FIRMWARE_IDENTIFYING:
        return mesh_str(MESH_STR_FW_STATE_IDENTIFYING);
    case MESH_FIRMWARE_CHECKING:
        return mesh_str(MESH_STR_FW_STATE_CHECKING);
    case MESH_FIRMWARE_UP_TO_DATE:
        return mesh_str(MESH_STR_FW_STATE_UP_TO_DATE);
    case MESH_FIRMWARE_AVAILABLE:
        return mesh_str(MESH_STR_FW_STATE_AVAILABLE);
    case MESH_FIRMWARE_FAILED:
        return mesh_str(MESH_STR_FW_STATE_FAILED);
    case MESH_FIRMWARE_STATE_COUNT:
    default:
        return mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
    }
}

const char *mesh_firmware_blocker_reason(enum mesh_firmware_blocker blocker) {
    switch (blocker) {
    case MESH_FIRMWARE_BLOCKER_NO_RADIO:
        return mesh_str(MESH_STR_FW_NO_RADIO);
    case MESH_FIRMWARE_BLOCKER_UNKNOWN_BOARD:
        return mesh_str(MESH_STR_FW_BLOCK_UNKNOWN_BOARD);
    case MESH_FIRMWARE_BLOCKER_AMBIGUOUS:
        return mesh_str(MESH_STR_FW_BLOCK_AMBIGUOUS);
    case MESH_FIRMWARE_BLOCKER_UNSUPPORTED_BOARD:
        return mesh_str(MESH_STR_FW_BLOCK_UNSUPPORTED);
    case MESH_FIRMWARE_BLOCKER_NO_PATH:
        return mesh_str(MESH_STR_FW_BLOCK_NO_PATH);
    case MESH_FIRMWARE_BLOCKER_WRONG_BUS:
        /* Which bus to go and use is a property of the board, not of the blocker, so the row
           that knows the board says it; this is the answer for a caller that has only the
           blocker to hand. */
        return mesh_str(MESH_STR_FW_BLOCK_NO_PATH);
    case MESH_FIRMWARE_BLOCKER_NONE:
    case MESH_FIRMWARE_BLOCKER_COUNT:
    default:
        return "";
    }
}

const struct mesh_firmware_board *mesh_firmware_board(const struct mesh_firmware *firmware) {
    if (firmware == NULL || firmware->boards.found != 1U || firmware->boards.count != 1U) {
        return NULL;
    }
    return &firmware->boards.entries[0];
}

static void firmware_set(struct mesh_firmware *firmware, enum mesh_firmware_state state,
                         const char *message) {
    firmware->state = state;
    mesh_str_copy(firmware->message, sizeof firmware->message, message != NULL ? message : "");
    firmware->revision++;
}

/*
 * Which of the refusals is true, decided most fundamental first.
 *
 * Recomputed rather than recorded, from the board and the bus, so unplugging a radio and
 * plugging it into the other port changes the answer without a check having to be pressed
 * again. That is the same rule the nav's back arrow and the map's selection follow: a second
 * opinion about a fact already on hand is a second opinion that can be wrong.
 */
static enum mesh_firmware_blocker firmware_blocker(const struct mesh_firmware *firmware) {
    if (firmware->bus == MESH_FIRMWARE_PATH_NONE) {
        return MESH_FIRMWARE_BLOCKER_NO_RADIO;
    }
    if (firmware->boards.found == 0U) {
        return MESH_FIRMWARE_BLOCKER_UNKNOWN_BOARD;
    }
    if (firmware->boards.found > 1U) {
        return MESH_FIRMWARE_BLOCKER_AMBIGUOUS;
    }
    const struct mesh_firmware_board *const board = &firmware->boards.entries[0];
    if (!board->actively_supported) {
        return MESH_FIRMWARE_BLOCKER_UNSUPPORTED_BOARD;
    }
    if (board->path == MESH_FIRMWARE_PATH_NONE) {
        return MESH_FIRMWARE_BLOCKER_NO_PATH;
    }
    if (board->path != firmware->bus) {
        return MESH_FIRMWARE_BLOCKER_WRONG_BUS;
    }
    return MESH_FIRMWARE_BLOCKER_NONE;
}

static void firmware_recompute_blocker(struct mesh_firmware *firmware) {
    const enum mesh_firmware_blocker blocker = firmware_blocker(firmware);
    if (blocker != firmware->blocker) {
        firmware->blocker = blocker;
        firmware->revision++;
    }
}

/*
 * The one line a failed check shows.
 *
 * `what` names the document that did not arrive, so the two failures are told apart: a
 * hardware list that will not read leaves the version question answerable, and a release index
 * that will not read leaves the board question answered. Both are still one line, because a
 * row is one line.
 */
static void firmware_fetch_failed(struct mesh_firmware *firmware,
                                  const struct mesh_fetch_result *result, enum mesh_str_id what) {
    char message[MESH_FIRMWARE_MESSAGE_MAX];
    switch (result->outcome) {
    case MESH_FETCH_TOO_LARGE:
    case MESH_FETCH_READ_FAILED:
        /* Which document, not which pipe: a reply past the cap and a pipe that failed are one
           answer to the reader - the list did not arrive - and `what` already says which list. */
        mesh_str_copy(message, sizeof message, mesh_str(what));
        break;
    case MESH_FETCH_TIMED_OUT:
        mesh_str_copy(message, sizeof message, mesh_str(MESH_STR_FW_TIMED_OUT));
        break;
    case MESH_FETCH_EXITED:
    case MESH_FETCH_OK:
    case MESH_FETCH_OUTCOME_COUNT:
    default:
        if (strcmp(mesh_fetch_tool(&firmware->fetch), "curl") == 0 && result->status == 60) {
            /* Exit 60 is "peer certificate cannot be authenticated", which on a device with no
               CA store at all is the only thing that will ever happen - and the bundle ships in
               the pak rather than through self-update, so the answer is to reinstall it. Which
               of the two it is only fits in the log; the row gets the short form. */
            mesh_str_copy(message, sizeof message,
                          mesh_str(firmware->fetch.ca_bundle[0] != '\0'
                                       ? MESH_STR_FW_TLS_UNVERIFIED
                                       : MESH_STR_FW_NO_CA_BUNDLE));
        } else {
            mesh_str_format(message, sizeof message, MESH_STR_FW_CHECK_EXIT,
                            mesh_fetch_tool(&firmware->fetch), result->status);
        }
        break;
    }
    /* The whole of it in the log, where a sentence has room: the row gets the short form above,
       and which outcome it was is the part that only ever helps somebody reading a log. */
    mesh_log_warn("firmware", "%s failed: %s (%s exit %d)", mesh_str(what), message,
                  mesh_fetch_tool(&firmware->fetch), result->status);
    firmware_set(firmware, MESH_FIRMWARE_FAILED, message);
}

static void firmware_on_hardware(void *userdata, const struct mesh_fetch_result *result);
static void firmware_on_index(void *userdata, const struct mesh_fetch_result *result);

/*
 * Starts the second half. Called both from the press and from the first half's completion,
 * which the fetcher allows: it is idle by the time a callback runs.
 *
 * The clock is `firmware->now_ms` rather than a parameter because a completion has no time to
 * hand: it arrives on the read path, and the deadline it sets has to be measured against the
 * same clock the tick that enforces it uses. Stamped at every check and every tick, so the
 * worst it is ever out by is one turn of the loop.
 */
static int firmware_start_index(struct mesh_firmware *firmware) {
    const struct mesh_fetch_request request = {
        .url = firmware_list_url(),
        .headers = {"Accept: application/json"},
        .timeout_ms = MESH_FIRMWARE_TIMEOUT_MS,
        .response_max = MESH_FIRMWARE_LIST_MAX,
        .on_done = firmware_on_index,
        .userdata = firmware,
    };
    const int result = mesh_fetch_start(&firmware->fetch, &request, firmware->now_ms);
    if (result != 0) {
        firmware_set(firmware, MESH_FIRMWARE_FAILED, mesh_str(MESH_STR_UPDATE_START_FAILED));
        return result;
    }
    firmware_set(firmware, MESH_FIRMWARE_CHECKING, mesh_str(MESH_STR_FW_STATE_CHECKING));
    return 0;
}

static void firmware_on_hardware(void *userdata, const struct mesh_fetch_result *result) {
    struct mesh_firmware *firmware = (struct mesh_firmware *)userdata;
    if (firmware == NULL || firmware->state != MESH_FIRMWARE_IDENTIFYING) {
        return;
    }
    if (result->outcome != MESH_FETCH_OK || result->body == NULL) {
        firmware_fetch_failed(firmware, result, MESH_STR_FW_HARDWARE_UNREADABLE);
        return;
    }
    if (!mesh_firmware_boards_parse(result->body, result->len, firmware->hw_model,
                                    &firmware->boards)) {
        firmware_set(firmware, MESH_FIRMWARE_FAILED, mesh_str(MESH_STR_FW_HARDWARE_UNREADABLE));
        return;
    }
    if (firmware->boards.found == 1U) {
        mesh_log_info("firmware", "hw_model %u is %s (%s, %s)", (unsigned)firmware->hw_model,
                      firmware->boards.entries[0].target, firmware->boards.entries[0].architecture,
                      firmware->boards.entries[0].actively_supported ? "supported" : "retired");
    } else {
        mesh_log_info("firmware", "hw_model %u matches %u boards", (unsigned)firmware->hw_model,
                      (unsigned)firmware->boards.found);
    }
    /* Straight on to the index, from inside this completion: the two documents are one press
       and a state in between that said "identified, now ask again" would be a row nobody could
       act on. */
    (void)firmware_start_index(firmware);
}

static void firmware_on_index(void *userdata, const struct mesh_fetch_result *result) {
    struct mesh_firmware *firmware = (struct mesh_firmware *)userdata;
    if (firmware == NULL || firmware->state != MESH_FIRMWARE_CHECKING) {
        return;
    }
    if (result->outcome != MESH_FETCH_OK || result->body == NULL) {
        firmware_fetch_failed(firmware, result, MESH_STR_FW_INDEX_UNREADABLE);
        return;
    }
    if (!mesh_firmware_release_parse(result->body, result->len, firmware->channel,
                                     &firmware->release)) {
        firmware_set(firmware, MESH_FIRMWARE_FAILED, mesh_str(MESH_STR_FW_INDEX_UNREADABLE));
        return;
    }

    firmware_recompute_blocker(firmware);
    mesh_log_info("firmware", "newest %s is %s (radio has %s)",
                  firmware->channel == MESH_FIRMWARE_CHANNEL_ALPHA ? "alpha" : "stable",
                  firmware->release.version,
                  firmware->running[0] != '\0' ? firmware->running : "?");

    if (mesh_firmware_version_compare(firmware->running, firmware->release.version) >= 0) {
        firmware_set(firmware, MESH_FIRMWARE_UP_TO_DATE, mesh_str(MESH_STR_FW_STATE_UP_TO_DATE));
        return;
    }
    /*
     * The version, on its own. Not "%s available (radio has %s)": the row above this one
     * already says what the radio is running, and a value column two dozen cells wide turns a
     * sentence into "2.7.26.54e0d8d available (" - which is what the row's *label* changing to
     * "Newer firmware" is for.
     */
    firmware_set(firmware, MESH_FIRMWARE_AVAILABLE, firmware->release.version);
}

int mesh_firmware_init(struct mesh_firmware *firmware, struct mesh_event_loop *loop) {
    if (firmware == NULL) {
        return -EINVAL;
    }
    memset(firmware, 0, sizeof *firmware);
    firmware->state = MESH_FIRMWARE_IDLE;
    firmware->channel = MESH_FIRMWARE_CHANNEL_STABLE;
    firmware->bus = MESH_FIRMWARE_PATH_NONE;
    firmware->blocker = MESH_FIRMWARE_BLOCKER_NO_RADIO;
    return mesh_fetch_init(&firmware->fetch, loop);
}

void mesh_firmware_shutdown(struct mesh_firmware *firmware) {
    if (firmware == NULL) {
        return;
    }
    mesh_fetch_shutdown(&firmware->fetch);
}

void mesh_firmware_use_ca_bundle(struct mesh_firmware *firmware, const char *path) {
    if (firmware == NULL) {
        return;
    }
    mesh_fetch_set_ca_bundle(&firmware->fetch, path);
}

bool mesh_firmware_available(const struct mesh_firmware *firmware) {
    return firmware != NULL && mesh_fetch_available(&firmware->fetch);
}

bool mesh_firmware_busy(const struct mesh_firmware *firmware) {
    return firmware != NULL && mesh_fetch_busy(&firmware->fetch);
}

void mesh_firmware_set_bus(struct mesh_firmware *firmware, enum mesh_firmware_path bus) {
    if (firmware == NULL || firmware->bus == bus) {
        return;
    }
    firmware->bus = bus;
    firmware->revision++;
    firmware_recompute_blocker(firmware);
}

bool mesh_firmware_answers_for(const struct mesh_firmware *firmware, uint32_t hw_model,
                               const char *running) {
    if (firmware == NULL || firmware->state == MESH_FIRMWARE_IDLE) {
        return true;
    }
    return firmware->hw_model == hw_model &&
           strcmp(firmware->running, running != NULL ? running : "") == 0;
}

void mesh_firmware_forget(struct mesh_firmware *firmware) {
    if (firmware == NULL || mesh_firmware_busy(firmware)) {
        return;
    }
    memset(&firmware->boards, 0, sizeof firmware->boards);
    memset(&firmware->release, 0, sizeof firmware->release);
    firmware->hw_model = 0U;
    firmware->running[0] = '\0';
    firmware_recompute_blocker(firmware);
    firmware_set(firmware, MESH_FIRMWARE_IDLE, "");
}

int mesh_firmware_check(struct mesh_firmware *firmware, uint32_t hw_model, const char *running,
                        uint64_t now_ms) {
    if (firmware == NULL) {
        return -EINVAL;
    }
    if (!mesh_firmware_available(firmware)) {
        return -ENOTSUP;
    }
    if (mesh_firmware_busy(firmware)) {
        return -EBUSY;
    }

    memset(&firmware->boards, 0, sizeof firmware->boards);
    memset(&firmware->release, 0, sizeof firmware->release);
    firmware->hw_model = hw_model;
    mesh_str_copy(firmware->running, sizeof firmware->running, running != NULL ? running : "");
    firmware->now_ms = now_ms;
    /* The old answer is gone, so the refusal that went with it is too - recomputed now rather
       than when the documents land, or the rows would keep naming last check's board while
       this one runs. */
    firmware_recompute_blocker(firmware);

    /*
     * A radio that has not said what it is skips the hardware list entirely. It is 39 KB whose
     * every answer is keyed on a model number we do not have, and fetching it to learn nothing
     * is a handheld's data allowance spent on a foregone conclusion. The release question is
     * still worth asking, so the check goes straight to the second document.
     */
    if (hw_model == 0U) {
        return firmware_start_index(firmware);
    }

    const struct mesh_fetch_request request = {
        .url = firmware_hardware_url(),
        .headers = {"Accept: application/json"},
        .timeout_ms = MESH_FIRMWARE_TIMEOUT_MS,
        .response_max = MESH_FIRMWARE_HARDWARE_MAX,
        .on_done = firmware_on_hardware,
        .userdata = firmware,
    };
    const int result = mesh_fetch_start(&firmware->fetch, &request, now_ms);
    if (result != 0) {
        firmware_set(firmware, MESH_FIRMWARE_FAILED, mesh_str(MESH_STR_UPDATE_START_FAILED));
        return result;
    }
    firmware_set(firmware, MESH_FIRMWARE_IDENTIFYING, mesh_str(MESH_STR_FW_STATE_IDENTIFYING));
    return 0;
}

void mesh_firmware_tick(struct mesh_firmware *firmware, uint64_t now_ms) {
    if (firmware == NULL) {
        return;
    }
    /* Stamped before the tick, so a completion the tick dispatches - which may start the second
       document - sets its deadline against this turn rather than the last one. */
    firmware->now_ms = now_ms;
    mesh_fetch_tick(&firmware->fetch, now_ms);
}
