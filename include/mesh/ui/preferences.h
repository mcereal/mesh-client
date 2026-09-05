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

struct mesh_ui_preferences {
    char preferred_device[64];
    /* Which transport preferred_device names (enum mesh_ui_device_kind). Without it a BLE
       address and a tty path are indistinguishable, and reconnecting would hand one to the
       wrong link. */
    uint8_t preferred_device_kind;
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
/* True when node_num is one of the radios we have connected to. */
bool mesh_ui_preferences_knows_radio(const struct mesh_ui_preferences *prefs, uint32_t node_num);

#ifdef __cplusplus
}
#endif
