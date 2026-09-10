#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How many radios of our own we remember having been connected to. Small on purpose: this is
   a most-recently-used list, and its only job is to keep the handful of nodes you actually
   own near the top of the Nodes tab. */
#define MESH_UI_MAX_KNOWN_RADIOS 8

/* How many radios we remember having connected to. The same size and the same reasoning as
   known_radios below: this is the handful of nodes you actually own. */
#define MESH_UI_MAX_KNOWN_DEVICES 8

/* One entry of the most-recently-used device list: how to reach a radio, and over what. */
struct mesh_ui_known_device {
    /* A BLE address, or a tty path / sysfs id for a USB port. */
    char identifier[64];
    uint8_t kind; /* enum mesh_ui_device_kind */
};

struct mesh_ui_preferences {
    char preferred_device[64];
    /* Which transport preferred_device names (enum mesh_ui_device_kind). Without it a BLE
       address and a tty path are indistinguishable, and reconnecting would hand one to the
       wrong link. */
    uint8_t preferred_device_kind;
    /*
     * Every radio we have connected to, most recent first - preferred_device is simply its
     * head, kept as its own field because that is what the file has always carried.
     *
     * One remembered radio is enough only for somebody who owns one. With two, walking out of
     * the house with the second means the saved node is the one that is not coming, and the
     * client had nothing better to fall back on than the loudest advertiser in the street.
     * A list lets auto-connect ask the question that actually matters - which of *my* radios
     * is in earshot right now - and answer it with the one used most recently.
     */
    struct mesh_ui_known_device known_devices[MESH_UI_MAX_KNOWN_DEVICES];
    uint8_t known_device_count;
    char preferred_channel[64];
    /* Node numbers of the radios this client has connected to, most recent first. A favorite
       lives in the connected radio's NodeDB, so pinning a node teaches that radio and nothing
       else; swap the Brick onto a different node and every pin you made is on the radio you
       just unplugged. This list is the client's own memory of your hardware, which is what
       lets mesh_app_node_rank() keep the radio you were using yesterday inside the Nodes
       tab's budget today. */
    uint32_t known_radios[MESH_UI_MAX_KNOWN_RADIOS];
    uint8_t known_radio_count;
    /* Which releases the self-updater is willing to be offered (enum mesh_update_channel,
       mesh/updater.h), carried as a byte so this header does not pull the updater in. 0 is
       DEFAULT, which is what a prefs file written before the setting existed reads as - so
       the inference the updater used to make on its own stays in force until someone picks. */
    uint8_t update_channel;
    /* Which look the UI is drawn with (a theme id from src/ui/theme.c, e.g. "light"). Empty
       means nobody has picked, which is what a prefs file written before the setting existed
       reads as - the default theme then applies, exactly as before. Stored by name rather
       than by index so reordering the theme table cannot move somebody onto another one. */
    char theme[16];
    /* Locale id, e.g. "es". Empty follows the system language. */
    char language[16];
    /* Whether a build that is not an official release may install what the updater finds.
       Remembered so the choice survives a relaunch: the alternative was an environment
       variable, which on a handheld means having a computer and an ssh session to hand. */
    bool update_allow_dev;
};

int mesh_ui_preferences_default_path(char *buffer, size_t buffer_len);
int mesh_ui_preferences_load(struct mesh_ui_preferences *prefs, const char *path);
int mesh_ui_preferences_save(const struct mesh_ui_preferences *prefs, const char *path);

/* Records node_num as a radio of ours, moving it to the front of the MRU list. Returns true
   when the list changed and the file therefore needs rewriting. Node 0 is not a node. */
bool mesh_ui_preferences_note_radio(struct mesh_ui_preferences *prefs, uint32_t node_num);

/* Records that we are on this device: it becomes preferred_device and the head of the
   most-recently-used list. Returns true when anything changed, so the caller knows to rewrite
   the file. This is the only way either is written - a second writer is a second opinion about
   which radio you were last on. */
bool mesh_ui_preferences_note_device(struct mesh_ui_preferences *prefs, const char *identifier,
                                     uint8_t kind);
/* Drops a device from the list, for when its pairing is forgotten. Returns true when it was
   there. Forgetting the head clears preferred_device with it. */
bool mesh_ui_preferences_forget_device(struct mesh_ui_preferences *prefs, const char *identifier,
                                       uint8_t kind);
/* How recently this device was used: 0 is the most recent, and -1 is "not one of ours". The
   ordering is the whole value here, which is why it is a rank rather than a bool. */
int mesh_ui_preferences_device_rank(const struct mesh_ui_preferences *prefs, const char *identifier,
                                    uint8_t kind);
/* True when node_num is one of the radios we have connected to. */
bool mesh_ui_preferences_knows_radio(const struct mesh_ui_preferences *prefs, uint32_t node_num);

#ifdef __cplusplus
}
#endif
