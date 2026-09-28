#include "mesh/ui/repeater_commands.h"

#include "inkwell/base/array.h"

/* Checked against the companion-v1.17.1 firmware's CommonCLI.cpp. Asking first, so the list
   reads down from what the repeater is to what it can be told to do. */
static const char *const k_commands[] = {
    "ver",                 /* its firmware version and build date */
    "clock",               /* the time it thinks it is */
    "clock sync",          /* set its clock from ours; it never goes backwards */
    "get radio",           /* frequency, bandwidth, spreading factor, coding rate */
    "get tx",              /* transmit power in dBm */
    "get repeat",          /* whether it relays at all */
    "get advert.interval", /* minutes between its zero-hop adverts */
    "neighbors",           /* who it hears directly, by key prefix, age and SNR */
    "advert",              /* announce itself across the mesh now */
};

size_t mesh_ui_repeater_command_count(void) { return INKWELL_ARRAY_LEN(k_commands); }

const char *mesh_ui_repeater_command(size_t index) {
    return index < INKWELL_ARRAY_LEN(k_commands) ? k_commands[index] : "";
}
