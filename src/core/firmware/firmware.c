#define _POSIX_C_SOURCE 200809L

#include "mesh/core/firmware.h"

#include "inkwell/base/env.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"

#include "mesh/i18n/net_reason.h"
#include "mesh/i18n/strings.h"

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

/*
 * MeshCore's three. The flasher's list is 138 KB for 67 devices; the tag list is a few KB; one
 * release is **650 KB**, because GitHub describes each of its 315 files - uploader and all - and
 * there is no asking for one file's entry. It is read once per check and freed, like the rest.
 */
#ifndef MESHCLIENT_FIRMWARE_MESHCORE_CONFIG_URL
#define MESHCLIENT_FIRMWARE_MESHCORE_CONFIG_URL "https://flasher.meshcore.io/config.json"
#endif
#ifndef MESHCLIENT_FIRMWARE_MESHCORE_TAGS_URL
#define MESHCLIENT_FIRMWARE_MESHCORE_TAGS_URL                                                      \
    "https://api.github.com/repos/meshcore-dev/MeshCore/git/matching-refs/tags/companion-v"
#endif
/* The tag is appended. */
#ifndef MESHCLIENT_FIRMWARE_MESHCORE_RELEASE_URL
#define MESHCLIENT_FIRMWARE_MESHCORE_RELEASE_URL                                                   \
    "https://api.github.com/repos/meshcore-dev/MeshCore/releases/tags/"
#endif

#define MESH_FIRMWARE_MESHCORE_CONFIG_MAX (512U * 1024U)
#define MESH_FIRMWARE_MESHCORE_TAGS_MAX (128U * 1024U)
#define MESH_FIRMWARE_MESHCORE_RELEASE_MAX (2U * 1024U * 1024U)

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

static const char *firmware_url(const char *name, const char *fallback) {
    const char *const from_env = getenv(name);
    return (from_env != NULL && from_env[0] != '\0') ? from_env : fallback;
}

const char *mesh_firmware_state_name(enum mesh_firmware_state state) {
    switch (state) {
    case MESH_FIRMWARE_IDLE:
        return inkcell_str(MESH_STR_FW_STATE_IDLE);
    case MESH_FIRMWARE_IDENTIFYING:
        return inkcell_str(MESH_STR_FW_STATE_IDENTIFYING);
    case MESH_FIRMWARE_CHECKING:
        return inkcell_str(MESH_STR_FW_STATE_CHECKING);
    case MESH_FIRMWARE_UP_TO_DATE:
        return inkcell_str(MESH_STR_FW_STATE_UP_TO_DATE);
    case MESH_FIRMWARE_AVAILABLE:
        return inkcell_str(MESH_STR_FW_STATE_AVAILABLE);
    case MESH_FIRMWARE_FAILED:
        return inkcell_str(MESH_STR_FW_STATE_FAILED);
    case MESH_FIRMWARE_CHOOSING:
        return inkcell_str(MESH_STR_FW_STATE_CHOOSING);
    case MESH_FIRMWARE_STATE_COUNT:
    default:
        return inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT);
    }
}

const char *mesh_firmware_blocker_reason(enum mesh_firmware_blocker blocker) {
    switch (blocker) {
    case MESH_FIRMWARE_BLOCKER_NO_RADIO:
        return inkcell_str(MESH_STR_FW_NO_RADIO);
    case MESH_FIRMWARE_BLOCKER_UNKNOWN_BOARD:
        return inkcell_str(MESH_STR_FW_BLOCK_UNKNOWN_BOARD);
    case MESH_FIRMWARE_BLOCKER_AMBIGUOUS:
        return inkcell_str(MESH_STR_FW_BLOCK_AMBIGUOUS);
    case MESH_FIRMWARE_BLOCKER_UNSUPPORTED_BOARD:
        return inkcell_str(MESH_STR_FW_BLOCK_UNSUPPORTED);
    case MESH_FIRMWARE_BLOCKER_NO_PATH:
        return inkcell_str(MESH_STR_FW_BLOCK_NO_PATH);
    case MESH_FIRMWARE_BLOCKER_WRONG_BUS:
        /* Which bus to go and use is a property of the board, not of the blocker, so the row
           that knows the board says it; this is the answer for a caller that has only the
           blocker to hand. */
        return inkcell_str(MESH_STR_FW_BLOCK_NO_PATH);
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
    inkwell_str_copy(firmware->message, sizeof firmware->message, message != NULL ? message : "");
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
    /* A radio that says nothing is never the link's: the app offered it for a bridge port
       nothing is connected over, and holds the port while the answer stands. */
    if (!firmware->bus_connected && !firmware->blank) {
        return MESH_FIRMWARE_BLOCKER_NO_RADIO;
    }
    /*
     * Attached, but not over a bus firmware can travel on - a network link, today. It falls
     * through rather than answering here, so the board's own path decides: `board->path` can
     * never equal a NONE bus, so this lands on WRONG_BUS and the row names the bus to go and
     * use. That is a thing the reader can act on, where "no radio" would be a thing they can
     * see is untrue.
     */
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
    if (firmware->blank) {
        return MESH_FIRMWARE_BLOCKER_NONE;
    }
    /* A switch writes the whole flash, which only the USB path does: the BLE ones write an
       application, and would ask the running firmware to take it. */
    if (!mesh_firmware_board_takes(board, firmware->bus) ||
        (firmware->switching && firmware->bus != MESH_FIRMWARE_PATH_USB)) {
        return MESH_FIRMWARE_BLOCKER_WRONG_BUS;
    }
    /* Over USB only through the ROM, and the ROM only through a bridge. A board with another
       path is sent to it; one whose only path is USB - a MeshCore ESP32 - has none from here. */
    if (firmware->bus == MESH_FIRMWARE_PATH_USB && firmware->bus_native_usb &&
        mesh_firmware_architecture_uses_esp_rom(board->architecture)) {
        return board->path == MESH_FIRMWARE_PATH_USB ? MESH_FIRMWARE_BLOCKER_NO_PATH
                                                     : MESH_FIRMWARE_BLOCKER_WRONG_BUS;
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
                                  const struct inkwell_fetch_result *result, inkcell_str_id what) {
    char message[MESH_FIRMWARE_MESSAGE_MAX];
    switch (result->outcome) {
    case INKWELL_FETCH_TIMED_OUT:
        inkwell_str_copy(message, sizeof message, inkcell_str(MESH_STR_FW_TIMED_OUT));
        break;
    case INKWELL_FETCH_NETWORK:
        /* Which network failure, where inkwell could tell; see updater_fetch_failed(). */
        if (!mesh_net_reason_format(&result->failure, result->host, NULL, message,
                                    sizeof message)) {
            inkwell_str_copy(message, sizeof message, inkcell_str(MESH_STR_FW_UNREACHABLE));
        }
        break;
    case INKWELL_FETCH_TLS:
        inkwell_str_copy(message, sizeof message, inkcell_str(MESH_STR_FW_TLS_UNVERIFIED));
        break;
    case INKWELL_FETCH_HTTP_STATUS:
        inkcell_str_format(message, sizeof message, MESH_STR_FW_CHECK_HTTP, result->status);
        break;
    case INKWELL_FETCH_TOO_LARGE:
    case INKWELL_FETCH_PROTOCOL:
    case INKWELL_FETCH_FILE:
    case INKWELL_FETCH_OK:
    case INKWELL_FETCH_OUTCOME_COUNT:
    default:
        /* Which document, not what was wrong with it: a reply past the cap and a reply that was
           not HTTP are one answer to the reader - the list did not arrive - and `what` already
           says which list. */
        inkwell_str_copy(message, sizeof message, inkcell_str(what));
        break;
    }
    /* The whole of it in the log, where a sentence has room: the row gets the short form above,
       and which host and which error it was is the part that only ever helps somebody reading a
       log. */
    inkwell_log_warn("firmware", "%s failed: %s (%s/%s: %s)", inkcell_str(what), message,
                     inkwell_fetch_outcome_name(result->outcome),
                     inkwell_net_reason_name(result->failure.reason), result->detail);
    firmware_set(firmware, MESH_FIRMWARE_FAILED, message);
}

static void firmware_conclude(struct mesh_firmware *firmware);
static void firmware_on_hardware(void *userdata, const struct inkwell_fetch_result *result);
static void firmware_on_index(void *userdata, const struct inkwell_fetch_result *result);

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
    const struct inkwell_fetch_request request = {
        .url = firmware_list_url(),
        .headers = {"Accept: application/json"},
        .timeout_ms = MESH_FIRMWARE_TIMEOUT_MS,
        .response_max = MESH_FIRMWARE_LIST_MAX,
        .on_done = firmware_on_index,
        .userdata = firmware,
    };
    const int result = inkwell_fetch_start(&firmware->fetch, &request, firmware->now_ms);
    if (result != 0) {
        firmware_set(firmware, MESH_FIRMWARE_FAILED, inkcell_str(MESH_STR_UPDATE_START_FAILED));
        return result;
    }
    firmware_set(firmware, MESH_FIRMWARE_CHECKING, inkcell_str(MESH_STR_FW_STATE_CHECKING));
    return 0;
}

static void firmware_on_hardware(void *userdata, const struct inkwell_fetch_result *result) {
    struct mesh_firmware *firmware = (struct mesh_firmware *)userdata;
    if (firmware == NULL || firmware->state != MESH_FIRMWARE_IDENTIFYING) {
        return;
    }
    if (result->outcome != INKWELL_FETCH_OK || result->body == NULL) {
        firmware_fetch_failed(firmware, result, MESH_STR_FW_HARDWARE_UNREADABLE);
        return;
    }
    const bool parsed = firmware->switching
                            ? mesh_firmware_boards_parse_target(result->body, result->len,
                                                                firmware->twin, &firmware->boards)
                            : mesh_firmware_boards_parse(result->body, result->len,
                                                         firmware->hw_model, &firmware->boards);
    if (!parsed) {
        firmware_set(firmware, MESH_FIRMWARE_FAILED, inkcell_str(MESH_STR_FW_HARDWARE_UNREADABLE));
        return;
    }
    if (firmware->boards.found == 1U) {
        inkwell_log_info("firmware", "hw_model %u is %s (%s, %s)",
                         (unsigned)firmware->boards.entries[0].hw_model,
                         firmware->boards.entries[0].target,
                         firmware->boards.entries[0].architecture,
                         firmware->boards.entries[0].actively_supported ? "supported" : "retired");
    } else {
        inkwell_log_info("firmware", "hw_model %u matches %u boards", (unsigned)firmware->hw_model,
                         (unsigned)firmware->boards.found);
    }
    /* Straight on to the index, from inside this completion: the two documents are one press
       and a state in between that said "identified, now ask again" would be a row nobody could
       act on. */
    (void)firmware_start_index(firmware);
}

static void firmware_on_index(void *userdata, const struct inkwell_fetch_result *result) {
    struct mesh_firmware *firmware = (struct mesh_firmware *)userdata;
    if (firmware == NULL || firmware->state != MESH_FIRMWARE_CHECKING) {
        return;
    }
    if (result->outcome != INKWELL_FETCH_OK || result->body == NULL) {
        firmware_fetch_failed(firmware, result, MESH_STR_FW_INDEX_UNREADABLE);
        return;
    }
    if (!mesh_firmware_release_parse(result->body, result->len, firmware->channel,
                                     &firmware->release)) {
        firmware_set(firmware, MESH_FIRMWARE_FAILED, inkcell_str(MESH_STR_FW_INDEX_UNREADABLE));
        return;
    }
    firmware->release.wipe = firmware->switching;

    inkwell_log_info("firmware", "newest %s is %s (radio has %s)",
                     firmware->channel == MESH_FIRMWARE_CHANNEL_ALPHA ? "alpha" : "stable",
                     firmware->release.version,
                     firmware->running[0] != '\0' ? firmware->running : "?");
    firmware_conclude(firmware);
}

/* The verdict, once a release is known: whichever source's documents named it. */
static void firmware_conclude(struct mesh_firmware *firmware) {
    /*
     * A switch is the whole flash through an ESP32's ROM, over USB, or nothing: the version is
     * news whatever it is, since the radio runs the other firmware and its numbers mean nothing
     * here. An nRF52 has no path yet - its UF2 writes the application and leaves the settings
     * the last firmware kept, so it would not start again as the new node a switch promises.
     */
    if (firmware->switching) {
        for (uint8_t i = 0; i < firmware->boards.count; ++i) {
            struct mesh_firmware_board *const board = &firmware->boards.entries[i];
            board->path = mesh_firmware_architecture_uses_esp_rom(board->architecture)
                              ? MESH_FIRMWARE_PATH_USB
                              : MESH_FIRMWARE_PATH_NONE;
        }
        firmware_recompute_blocker(firmware);
        firmware_set(firmware, MESH_FIRMWARE_AVAILABLE, firmware->release.version);
        return;
    }
    firmware_recompute_blocker(firmware);
    /* The other build is news at any version: the radio does not run it. */
    if (firmware->other_build) {
        firmware_set(firmware, MESH_FIRMWARE_AVAILABLE, firmware->release.version);
        return;
    }

    /*
     * MESHCLIENT_FIRMWARE_REINSTALL offers the release the radio is already running, which is
     * how an install path is tested again once it has worked - there is no newer image to try
     * it with. The same version and never an older one: a downgrade is a different question,
     * with a filesystem the older firmware may not read.
     */
    const int compared =
        mesh_firmware_version_compare(firmware->running, firmware->release.version);
    if (compared > 0 ||
        (compared == 0 && !inkwell_env_bool("FIRMWARE_REINSTALL", "firmware reinstall", false))) {
        firmware_set(firmware, MESH_FIRMWARE_UP_TO_DATE, inkcell_str(MESH_STR_FW_STATE_UP_TO_DATE));
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

int mesh_firmware_init(struct mesh_firmware *firmware, struct inkwell_loop *loop) {
    if (firmware == NULL) {
        return -EINVAL;
    }
    memset(firmware, 0, sizeof *firmware);
    firmware->state = MESH_FIRMWARE_IDLE;
    firmware->channel = MESH_FIRMWARE_CHANNEL_STABLE;
    firmware->bus = MESH_FIRMWARE_PATH_NONE;
    firmware->blocker = MESH_FIRMWARE_BLOCKER_NO_RADIO;
    return inkwell_fetch_init(&firmware->fetch, loop);
}

void mesh_firmware_shutdown(struct mesh_firmware *firmware) {
    if (firmware == NULL) {
        return;
    }
    inkwell_fetch_shutdown(&firmware->fetch);
}

bool mesh_firmware_available(const struct mesh_firmware *firmware) {
    return firmware != NULL && inkwell_fetch_available(&firmware->fetch);
}

bool mesh_firmware_busy(const struct mesh_firmware *firmware) {
    return firmware != NULL && inkwell_fetch_busy(&firmware->fetch);
}

void mesh_firmware_set_bus(struct mesh_firmware *firmware, enum mesh_firmware_path bus,
                           bool connected) {
    if (firmware == NULL || (firmware->bus == bus && firmware->bus_connected == connected)) {
        return;
    }
    firmware->bus = bus;
    firmware->bus_connected = connected;
    firmware->revision++;
    firmware_recompute_blocker(firmware);
}

void mesh_firmware_set_bus_native_usb(struct mesh_firmware *firmware, bool native) {
    if (firmware == NULL || firmware->bus_native_usb == native) {
        return;
    }
    firmware->bus_native_usb = native;
    firmware->revision++;
    firmware_recompute_blocker(firmware);
}

const char *mesh_firmware_channel_name(enum mesh_firmware_channel channel) {
    /* Upstream's own words for its own lists, so they stay as they are - the rule region codes
       and modem preset names already follow. */
    return channel == MESH_FIRMWARE_CHANNEL_ALPHA ? "alpha" : "stable";
}

bool mesh_firmware_set_channel(struct mesh_firmware *firmware, enum mesh_firmware_channel channel) {
    if (firmware == NULL || channel >= MESH_FIRMWARE_CHANNEL_COUNT ||
        firmware->channel == channel) {
        return false;
    }
    if (mesh_firmware_busy(firmware)) {
        /* Mid-check: the document being read was asked for under the old channel, and letting
           the answer land against the new one would report an alpha as the newest stable. */
        return false;
    }
    firmware->channel = channel;
    mesh_firmware_forget(firmware);
    inkwell_log_info("firmware", "Firmware channel set to %s", mesh_firmware_channel_name(channel));
    return true;
}

enum mesh_firmware_source mesh_firmware_radio_source(const struct mesh_firmware *firmware) {
    if (!firmware->switching) {
        return firmware->source;
    }
    return firmware->source == MESH_FIRMWARE_SOURCE_MESHCORE ? MESH_FIRMWARE_SOURCE_MESHTASTIC
                                                             : MESH_FIRMWARE_SOURCE_MESHCORE;
}

bool mesh_firmware_answers_for(const struct mesh_firmware *firmware, uint32_t hw_model,
                               const char *running) {
    if (firmware == NULL || firmware->state == MESH_FIRMWARE_IDLE) {
        return true;
    }
    return mesh_firmware_radio_source(firmware) == MESH_FIRMWARE_SOURCE_MESHTASTIC &&
           firmware->hw_model == hw_model &&
           strcmp(firmware->running, running != NULL ? running : "") == 0;
}

bool mesh_firmware_answers_for_meshcore(const struct mesh_firmware *firmware, const char *model,
                                        const char *running, bool usb_build) {
    if (firmware == NULL || firmware->state == MESH_FIRMWARE_IDLE) {
        return true;
    }
    /* The Bluetooth build asked for over the cable is still about the USB build running. */
    return mesh_firmware_radio_source(firmware) == MESH_FIRMWARE_SOURCE_MESHCORE &&
           (firmware->usb_build || firmware->other_build) == usb_build &&
           strcmp(firmware->model, model != NULL ? model : "") == 0 &&
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
    firmware->model[0] = '\0';
    firmware->tag[0] = '\0';
    firmware->switching = false;
    firmware->other_build = false;
    firmware->twin[0] = '\0';
    firmware->blank = false;
    firmware->choices.count = 0U;
    firmware_recompute_blocker(firmware);
    firmware_set(firmware, MESH_FIRMWARE_IDLE, "");
}

/* ---- MeshCore's three documents ------------------------------------------------------------ */

static void firmware_on_meshcore_config(void *userdata, const struct inkwell_fetch_result *result);
static void firmware_on_meshcore_tags(void *userdata, const struct inkwell_fetch_result *result);
static void firmware_on_meshcore_release(void *userdata, const struct inkwell_fetch_result *result);

static int firmware_get(struct mesh_firmware *firmware, const char *url, const char *accept,
                        size_t max, inkwell_fetch_done_fn on_done) {
    const struct inkwell_fetch_request request = {
        .url = url,
        .headers = {accept},
        .timeout_ms = MESH_FIRMWARE_TIMEOUT_MS,
        .response_max = max,
        .on_done = on_done,
        .userdata = firmware,
    };
    const int result = inkwell_fetch_start(&firmware->fetch, &request, firmware->now_ms);
    if (result != 0) {
        firmware_set(firmware, MESH_FIRMWARE_FAILED, inkcell_str(MESH_STR_UPDATE_START_FAILED));
    }
    return result;
}

static void firmware_on_meshcore_config(void *userdata, const struct inkwell_fetch_result *result) {
    struct mesh_firmware *firmware = (struct mesh_firmware *)userdata;
    if (firmware == NULL || firmware->state != MESH_FIRMWARE_IDENTIFYING) {
        return;
    }
    if (result->outcome != INKWELL_FETCH_OK || result->body == NULL) {
        firmware_fetch_failed(firmware, result, MESH_STR_FW_HARDWARE_UNREADABLE);
        return;
    }
    /* A radio that said nothing, and nobody has chosen what it is yet: the list to choose
       from is the whole answer. */
    if (firmware->blank && firmware->twin[0] == '\0') {
        if (!mesh_firmware_meshcore_usb_devices(result->body, result->len, &firmware->choices)) {
            firmware_set(firmware, MESH_FIRMWARE_FAILED,
                         inkcell_str(MESH_STR_FW_HARDWARE_UNREADABLE));
            return;
        }
        inkwell_log_info("firmware", "%u MeshCore boards take a USB companion",
                         (unsigned)firmware->choices.count);
        firmware_set(firmware, MESH_FIRMWARE_CHOOSING, "");
        return;
    }
    /* A switch looks the board up by the flasher's own name for it, and wants the USB
       companion: the build a radio on this cable can still be reached by afterwards. */
    const char *const name = firmware->switching ? firmware->twin : firmware->model;
    if (!mesh_firmware_meshcore_boards_parse(result->body, result->len, name,
                                             firmware->switching || firmware->usb_build,
                                             &firmware->boards)) {
        firmware_set(firmware, MESH_FIRMWARE_FAILED, inkcell_str(MESH_STR_FW_HARDWARE_UNREADABLE));
        return;
    }
    if (firmware->boards.found == 1U) {
        inkwell_log_info("firmware", "\"%s\" is %s (%s)", name, firmware->boards.entries[0].target,
                         firmware->boards.entries[0].architecture);
    } else {
        inkwell_log_info("firmware", "\"%s\" matches %u builds", name,
                         (unsigned)firmware->boards.found);
    }
    if (firmware_get(firmware,
                     firmware_url("MESHCLIENT_FIRMWARE_MESHCORE_TAGS_URL",
                                  MESHCLIENT_FIRMWARE_MESHCORE_TAGS_URL),
                     "Accept: application/vnd.github+json", MESH_FIRMWARE_MESHCORE_TAGS_MAX,
                     firmware_on_meshcore_tags) == 0) {
        firmware_set(firmware, MESH_FIRMWARE_CHECKING, inkcell_str(MESH_STR_FW_STATE_CHECKING));
    }
}

static void firmware_on_meshcore_tags(void *userdata, const struct inkwell_fetch_result *result) {
    struct mesh_firmware *firmware = (struct mesh_firmware *)userdata;
    if (firmware == NULL || firmware->state != MESH_FIRMWARE_CHECKING) {
        return;
    }
    if (result->outcome != INKWELL_FETCH_OK || result->body == NULL) {
        firmware_fetch_failed(firmware, result, MESH_STR_FW_INDEX_UNREADABLE);
        return;
    }
    if (!mesh_firmware_meshcore_latest_tag(result->body, result->len, firmware->tag,
                                           sizeof firmware->tag, firmware->release.version,
                                           sizeof firmware->release.version)) {
        firmware_set(firmware, MESH_FIRMWARE_FAILED, inkcell_str(MESH_STR_FW_INDEX_UNREADABLE));
        return;
    }
    inkwell_log_info("firmware", "newest MeshCore companion is %s (radio has %s)",
                     firmware->release.version,
                     firmware->running[0] != '\0' ? firmware->running : "?");
    /* The file is only worth looking for when there is one board to look for it for: an
       unknown or ambiguous radio gets its version answer without 650 KB it cannot use. */
    if (firmware->boards.found != 1U) {
        firmware_conclude(firmware);
        return;
    }
    char url[MESH_FIRMWARE_URL_MAX];
    snprintf(url, sizeof url, "%s%s",
             firmware_url("MESHCLIENT_FIRMWARE_MESHCORE_RELEASE_URL",
                          MESHCLIENT_FIRMWARE_MESHCORE_RELEASE_URL),
             firmware->tag);
    (void)firmware_get(firmware, url, "Accept: application/vnd.github+json",
                       MESH_FIRMWARE_MESHCORE_RELEASE_MAX, firmware_on_meshcore_release);
}

static void firmware_on_meshcore_release(void *userdata,
                                         const struct inkwell_fetch_result *result) {
    struct mesh_firmware *firmware = (struct mesh_firmware *)userdata;
    if (firmware == NULL || firmware->state != MESH_FIRMWARE_CHECKING) {
        return;
    }
    if (result->outcome != INKWELL_FETCH_OK || result->body == NULL) {
        firmware_fetch_failed(firmware, result, MESH_STR_FW_INDEX_UNREADABLE);
        return;
    }
    /* A release with no file for this build still has a version to report: the row says there
       is no download, the way a Meshtastic release with no manifest yet does. */
    firmware->release.wipe = firmware->switching;
    if (mesh_firmware_meshcore_asset_parse(result->body, result->len, &firmware->boards.entries[0],
                                           &firmware->release)) {
        inkwell_log_info("firmware", "The image is %s, %llu bytes", firmware->release.image_name,
                         (unsigned long long)firmware->release.image_bytes);
    } else {
        inkwell_log_warn("firmware", "%s publishes no file for %s", firmware->tag,
                         firmware->boards.entries[0].target);
    }
    firmware_conclude(firmware);
}

/* What both checks forget before they start: the last answer, and whose it was. */
static void firmware_begin(struct mesh_firmware *firmware, enum mesh_firmware_source source,
                           uint32_t hw_model, const char *model, const char *running,
                           uint64_t now_ms) {
    memset(&firmware->boards, 0, sizeof firmware->boards);
    memset(&firmware->release, 0, sizeof firmware->release);
    firmware->source = source;
    firmware->hw_model = hw_model;
    inkwell_str_copy(firmware->model, sizeof firmware->model, model != NULL ? model : "");
    inkwell_str_copy(firmware->running, sizeof firmware->running, running != NULL ? running : "");
    firmware->tag[0] = '\0';
    firmware->switching = false;
    firmware->other_build = false;
    firmware->twin[0] = '\0';
    firmware->blank = false;
    firmware->choices.count = 0U;
    firmware->now_ms = now_ms;
    /* The old answer is gone, so the refusal that went with it is too - recomputed now rather
       than when the documents land, or the rows would keep naming last check's board while
       this one runs. */
    firmware_recompute_blocker(firmware);
}

int mesh_firmware_check_meshcore(struct mesh_firmware *firmware, const char *model,
                                 const char *running, bool usb_build, uint64_t now_ms) {
    if (firmware == NULL) {
        return -EINVAL;
    }
    if (!mesh_firmware_available(firmware)) {
        return -ENOTSUP;
    }
    if (mesh_firmware_busy(firmware)) {
        return -EBUSY;
    }
    firmware_begin(firmware, MESH_FIRMWARE_SOURCE_MESHCORE, 0U, model, running, now_ms);
    firmware->usb_build = usb_build;
    if (firmware_get(firmware,
                     firmware_url("MESHCLIENT_FIRMWARE_MESHCORE_CONFIG_URL",
                                  MESHCLIENT_FIRMWARE_MESHCORE_CONFIG_URL),
                     "Accept: application/json", MESH_FIRMWARE_MESHCORE_CONFIG_MAX,
                     firmware_on_meshcore_config) != 0) {
        return -EIO;
    }
    firmware_set(firmware, MESH_FIRMWARE_IDENTIFYING, inkcell_str(MESH_STR_FW_STATE_IDENTIFYING));
    return 0;
}

int mesh_firmware_check_meshcore_bluetooth(struct mesh_firmware *firmware, const char *model,
                                           const char *running, uint64_t now_ms) {
    const int result = mesh_firmware_check_meshcore(firmware, model, running, false, now_ms);
    if (result == 0) {
        firmware->other_build = true;
    }
    return result;
}

/* Both switches: the same refusals as a check, and a board with no twin is -ENOENT. */
static int firmware_switch_ready(struct mesh_firmware *firmware, const char *twin) {
    if (firmware == NULL) {
        return -EINVAL;
    }
    if (twin == NULL) {
        return -ENOENT;
    }
    if (!mesh_firmware_available(firmware)) {
        return -ENOTSUP;
    }
    return mesh_firmware_busy(firmware) ? -EBUSY : 0;
}

int mesh_firmware_check_switch_to_meshcore(struct mesh_firmware *firmware, uint32_t hw_model,
                                           const char *running, const char *target,
                                           uint64_t now_ms) {
    const char *const device = mesh_firmware_meshcore_device_for_target(target);
    const int ready = firmware_switch_ready(firmware, device);
    if (ready != 0) {
        return ready;
    }
    firmware_begin(firmware, MESH_FIRMWARE_SOURCE_MESHCORE, hw_model, "", running, now_ms);
    firmware->switching = true;
    firmware->usb_build = false;
    inkwell_str_copy(firmware->twin, sizeof firmware->twin, device);
    inkwell_log_info("firmware", "%s is MeshCore's \"%s\"; looking for its build", target, device);
    if (firmware_get(firmware,
                     firmware_url("MESHCLIENT_FIRMWARE_MESHCORE_CONFIG_URL",
                                  MESHCLIENT_FIRMWARE_MESHCORE_CONFIG_URL),
                     "Accept: application/json", MESH_FIRMWARE_MESHCORE_CONFIG_MAX,
                     firmware_on_meshcore_config) != 0) {
        return -EIO;
    }
    firmware_set(firmware, MESH_FIRMWARE_IDENTIFYING, inkcell_str(MESH_STR_FW_STATE_IDENTIFYING));
    return 0;
}

int mesh_firmware_check_switch_to_meshtastic(struct mesh_firmware *firmware, const char *model,
                                             const char *running, bool usb_build,
                                             const char *device, uint64_t now_ms) {
    const char *const target = mesh_firmware_meshcore_target_for_device(device);
    const int ready = firmware_switch_ready(firmware, target);
    if (ready != 0) {
        return ready;
    }
    firmware_begin(firmware, MESH_FIRMWARE_SOURCE_MESHTASTIC, 0U, model, running, now_ms);
    firmware->switching = true;
    firmware->usb_build = usb_build;
    inkwell_str_copy(firmware->twin, sizeof firmware->twin, target);
    inkwell_log_info("firmware", "\"%s\" is Meshtastic's %s; looking for its build", device,
                     target);
    if (firmware_get(firmware, firmware_hardware_url(), "Accept: application/json",
                     MESH_FIRMWARE_HARDWARE_MAX, firmware_on_hardware) != 0) {
        return -EIO;
    }
    firmware_set(firmware, MESH_FIRMWARE_IDENTIFYING, inkcell_str(MESH_STR_FW_STATE_IDENTIFYING));
    return 0;
}

/* The flasher's list, which both halves of a blank radio start from. */
static int firmware_start_meshcore_config(struct mesh_firmware *firmware) {
    if (firmware_get(firmware,
                     firmware_url("MESHCLIENT_FIRMWARE_MESHCORE_CONFIG_URL",
                                  MESHCLIENT_FIRMWARE_MESHCORE_CONFIG_URL),
                     "Accept: application/json", MESH_FIRMWARE_MESHCORE_CONFIG_MAX,
                     firmware_on_meshcore_config) != 0) {
        return -EIO;
    }
    firmware_set(firmware, MESH_FIRMWARE_IDENTIFYING, inkcell_str(MESH_STR_FW_STATE_IDENTIFYING));
    return 0;
}

int mesh_firmware_list_blank(struct mesh_firmware *firmware, uint64_t now_ms) {
    const int ready = firmware_switch_ready(firmware, "");
    if (ready != 0) {
        return ready;
    }
    firmware_begin(firmware, MESH_FIRMWARE_SOURCE_MESHCORE, 0U, "", "", now_ms);
    firmware->blank = true;
    return firmware_start_meshcore_config(firmware);
}

int mesh_firmware_check_blank(struct mesh_firmware *firmware, const char *device, uint64_t now_ms) {
    const int ready =
        firmware_switch_ready(firmware, device != NULL && device[0] != '\0' ? device : NULL);
    if (ready != 0) {
        return ready;
    }
    /* Copied out first: `device` may be one of the `choices` the begin forgets. */
    char chosen[MESH_FIRMWARE_TARGET_MAX];
    inkwell_str_copy(chosen, sizeof chosen, device);
    firmware_begin(firmware, MESH_FIRMWARE_SOURCE_MESHCORE, 0U, "", "", now_ms);
    firmware->blank = true;
    firmware->switching = true;
    inkwell_str_copy(firmware->twin, sizeof firmware->twin, chosen);
    inkwell_log_info("firmware", "A silent radio chosen as \"%s\"; looking for its build", chosen);
    return firmware_start_meshcore_config(firmware);
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

    firmware_begin(firmware, MESH_FIRMWARE_SOURCE_MESHTASTIC, hw_model, "", running, now_ms);

    /*
     * A radio that has not said what it is skips the hardware list entirely. It is 39 KB whose
     * every answer is keyed on a model number we do not have, and fetching it to learn nothing
     * is a handheld's data allowance spent on a foregone conclusion. The release question is
     * still worth asking, so the check goes straight to the second document.
     */
    if (hw_model == 0U) {
        return firmware_start_index(firmware);
    }

    const struct inkwell_fetch_request request = {
        .url = firmware_hardware_url(),
        .headers = {"Accept: application/json"},
        .timeout_ms = MESH_FIRMWARE_TIMEOUT_MS,
        .response_max = MESH_FIRMWARE_HARDWARE_MAX,
        .on_done = firmware_on_hardware,
        .userdata = firmware,
    };
    const int result = inkwell_fetch_start(&firmware->fetch, &request, now_ms);
    if (result != 0) {
        firmware_set(firmware, MESH_FIRMWARE_FAILED, inkcell_str(MESH_STR_UPDATE_START_FAILED));
        return result;
    }
    firmware_set(firmware, MESH_FIRMWARE_IDENTIFYING, inkcell_str(MESH_STR_FW_STATE_IDENTIFYING));
    return 0;
}

void mesh_firmware_tick(struct mesh_firmware *firmware, uint64_t now_ms) {
    if (firmware == NULL) {
        return;
    }
    /* Stamped before the tick, so a completion the tick dispatches - which may start the second
       document - sets its deadline against this turn rather than the last one. */
    firmware->now_ms = now_ms;
    inkwell_fetch_tick(&firmware->fetch, now_ms);
}
