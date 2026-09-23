#ifndef MESH_UI_COMMANDS_H
#define MESH_UI_COMMANDS_H

/*
 * What the user can ask the active UI context to do.
 *
 * A command is deliberately not a key. The Brick may bind DELETE to X, a desktop toolbar may
 * draw a button labelled "Delete", and a future compact device may put it in an overflow menu;
 * all three are the same request. Keeping the command beside its current legacy button binding
 * lets the existing action bar remain unchanged while input backends are moved across this seam.
 */

#include "inkcell/ui/actions.h"

#include <stddef.h>

struct mesh_ui_snapshot;

enum mesh_ui_command_id {
    MESH_UI_COMMAND_NONE = 0,
    MESH_UI_COMMAND_ADDRESS,
    MESH_UI_COMMAND_ANSWER,
    MESH_UI_COMMAND_BACK,
    MESH_UI_COMMAND_CANCEL,
    MESH_UI_COMMAND_CHART,
    MESH_UI_COMMAND_CHOOSE,
    MESH_UI_COMMAND_CONFIRM,
    MESH_UI_COMMAND_CONFIRM_DELETE,
    MESH_UI_COMMAND_CONFIRM_DISCARD,
    MESH_UI_COMMAND_CONFIRM_FORGET,
    MESH_UI_COMMAND_CONFIRM_REMOVE,
    MESH_UI_COMMAND_CONNECT,
    MESH_UI_COMMAND_DELETE,
    MESH_UI_COMMAND_DISCARD,
    MESH_UI_COMMAND_DISCONNECT,
    MESH_UI_COMMAND_DONE,
    MESH_UI_COMMAND_EDIT,
    MESH_UI_COMMAND_FILTER,
    MESH_UI_COMMAND_FIT,
    MESH_UI_COMMAND_FORGET,
    MESH_UI_COMMAND_GROUPS,
    MESH_UI_COMMAND_HELP,
    MESH_UI_COMMAND_JUMP,
    MESH_UI_COMMAND_KEYS,
    MESH_UI_COMMAND_MOVE,
    MESH_UI_COMMAND_MUTE,
    MESH_UI_COMMAND_NEW,
    MESH_UI_COMMAND_OPEN,
    MESH_UI_COMMAND_PAIR,
    MESH_UI_COMMAND_PAN,
    MESH_UI_COMMAND_PIN,
    MESH_UI_COMMAND_QUIT,
    MESH_UI_COMMAND_REACT,
    MESH_UI_COMMAND_READINGS,
    MESH_UI_COMMAND_REFRESH,
    MESH_UI_COMMAND_REPLY,
    MESH_UI_COMMAND_RESEND,
    MESH_UI_COMMAND_RUN,
    MESH_UI_COMMAND_SAVE,
    MESH_UI_COMMAND_SCROLL,
    MESH_UI_COMMAND_SELECT,
    MESH_UI_COMMAND_SEND,
    MESH_UI_COMMAND_SHIFT,
    MESH_UI_COMMAND_SORT,
    MESH_UI_COMMAND_SPACE,
    MESH_UI_COMMAND_SPAN,
    MESH_UI_COMMAND_TABS,
    MESH_UI_COMMAND_TREND,
    MESH_UI_COMMAND_TYPE,
    MESH_UI_COMMAND_UNMUTE,
    MESH_UI_COMMAND_WRITE,
    MESH_UI_COMMAND_ZOOM_IN,
    MESH_UI_COMMAND_ZOOM_OUT,
    MESH_UI_COMMAND_COUNT
};

struct mesh_ui_command {
    enum mesh_ui_command_id id;
    inkcell_str_id label;
    /* The direct-controller binding used by the existing Brick UI. Presentation and dispatch
       may ignore it: it is a compatibility binding, not the command's identity. */
    enum inkcell_button button;
};

/* Commands are application data, not a rendered bar. This capacity leaves room for the builders
   to grow an overflow menu after the legacy-bar compatibility bridge is removed. */
#define MESH_UI_COMMANDS_MAX 16U

struct mesh_ui_command_set {
    struct mesh_ui_command items[MESH_UI_COMMANDS_MAX];
    size_t count;
};

/* The commands offered by the topmost active context, in presentation priority order. */
void mesh_ui_commands_for(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_command_set *out);

/* Lookup helpers for adapters and contract tests. */
const struct mesh_ui_command *mesh_ui_commands_find(const struct mesh_ui_command_set *set,
                                                    enum mesh_ui_command_id id);
const struct mesh_ui_command *mesh_ui_commands_find_button(const struct mesh_ui_command_set *set,
                                                           enum inkcell_button button);

#endif /* MESH_UI_COMMANDS_H */
