#include "mesh/utils/crash.h"

#include "inkwell/runtime/crash.h"

/* In slot order. inkwell pads them to the column the values line up in. */
static const char *const k_note_labels[MESH_CRASH_NOTE_SLOT_COUNT] = {
    [MESH_CRASH_NOTE_VERSION] = "version",
    [MESH_CRASH_NOTE_ROUTE] = "route",
    [MESH_CRASH_NOTE_TRANSPORT] = "transport",
};

int mesh_crash_install(const char *dir) {
    const struct inkwell_crash_config config = {
        .dir = dir,
        .product = "MeshClient",
        /* Lowercase, because it is what a reader runs addr2line against rather than what the
           screen calls the program. */
        .binary = "meshclient",
        .issues_url = "https://github.com/mcereal/mesh-client/issues",
        .log_warning =
            "Depending on what you were doing they can name nodes, channels and places,\n"
            "quote a message you sent, or hold a position you entered by hand. Channel\n"
            "keys are never written here.",
        .note_labels = k_note_labels,
        .note_count = (unsigned)MESH_CRASH_NOTE_SLOT_COUNT,
    };
    return inkwell_crash_install(&config);
}
