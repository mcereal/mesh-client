#define _POSIX_C_SOURCE 200809L

/*
 * The Devices tab: every radio this client can see, on whichever bus it was found on, and the
 * row for typing in the one address that is never discovered.
 *
 * What a device is *called* is decided here and read from the frame's status line too - see
 * fb_device_label() in fb_screens_internal.h. A radio named one way on its row and another way
 * under the keycaps would be two radios as far as the reader is concerned.
 */

#include "inkcell/ui/widgets.h"
#include "inkwell/base/text.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/devices.h"
#include "mesh/ui/focus.h"
#include "mesh/ui/nav.h"

#include <stdio.h>
#include <string.h>

/* What to call it: the advertised name when it has one, otherwise whatever we addressed it by. */
const char *fb_device_label(const struct mesh_ui_device *device) {
    return device->name[0] != '\0' ? device->name : device->identifier;
}

/*
 * The disc a device row carries: which bus the radio is on, in one place.
 *
 * `LINK` rather than a network glyph of its own on purpose. Adding an icon means regenerating
 * the whole sprite table out of Material Symbols, and upstream has moved since icon_glyphs.c
 * was last built, so every icon in the client would change in a transport change. The id is
 * honest in the meantime - what this row is, is the link itself, which is exactly the one kind
 * here that was never discovered and never advertised.
 */
static enum inkcell_icon fb_device_icon(const struct mesh_ui_device *device) {
    switch ((enum mesh_ui_device_kind)device->kind) {
    case MESH_UI_DEVICE_SERIAL:
        return INKCELL_ICON_USB;
    case MESH_UI_DEVICE_TCP:
        return INKCELL_ICON_LINK;
    case MESH_UI_DEVICE_BLE:
        break;
    }
    return INKCELL_ICON_BLUETOOTH;
}

/*
 * The Devices tab's last row: the network address, and the way to type one.
 *
 * It draws in the same three slots every other row of this list uses - a disc saying which bus,
 * the name, and a supporting line - so that the moment a network link comes up and discovery
 * publishes a real row in its place, nothing on the panel moves. What it does not carry is a
 * capsule: the badge slot reports what a link is *doing*, and this row is a button until an
 * address exists.
 */
static void fb_devices_network_row(struct inkcell_draw_state *state, struct inkcell_fb_list *list,
                                   uint32_t index, const struct mesh_ui_devices_row *entry) {
    const bool configured = (entry->host[0] != '\0');
    const struct inkcell_fb_list_item row = {
        .leading = {.kind = INKCELL_FB_LEADING_AVATAR,
                    .icon = INKCELL_ICON_LINK,
                    .tint = index,
                    .role = INKCELL_COLOR_COUNT},
        /* The address itself is the name once there is one, exactly as it is on the row
           discovery publishes for a live network link: a host has no advertisement coming, so
           what it is reachable at is what it is called. */
        .text = configured ? entry->host : inkcell_str(MESH_STR_DEVICES_NETWORK_ROW),
        /* Dim while there is nothing to connect to, for the reason the Nodes tab's map row is
           dim with no markers on it: it is a button among things, and one that does not yet
           lead anywhere. */
        .tone = configured ? INKCELL_TONE_NORMAL : INKCELL_TONE_DIM,
        .trailing = {.kind = INKCELL_FB_TRAILING_TEXT,
                     .text = configured ? inkcell_str(MESH_STR_DEVICES_TRAILING_NETWORK) : ""},
        .supporting = inkcell_str(configured ? MESH_STR_DEVICES_NETWORK_READY
                                             : MESH_STR_DEVICES_NETWORK_UNSET),
        .supporting_tone = INKCELL_TONE_DIM,
        .supporting_quiet = true,
        .divider = true,
    };
    inkcell_fb_list_item(state, list, index, &row);
}

/* Takes the state mutably, like every inkcell_fb_list_item() caller: the item is the component that
   can carry an animated slot, so the whole entry point takes the table it would step. */
void fb_render_devices(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                       struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    /* The heading counts the radios, not the rows: the network row is a control, and a Devices
       tab reading "Devices 1" with nothing found would be the arithmetic-no-screen-should-show
       rule the Nodes title states. */
    char title[96];
    inkcell_fb_title_count(title, sizeof title, inkcell_str(MESH_STR_TAB_DEVICES),
                           (uint32_t)snapshot->device_count, 0U);
    inkcell_fb_draw_app_bar(state, layout, &(const struct inkcell_fb_app_bar){.title = title});

    /*
     * Two body rows an item: the radio's name with how it is attached against the right edge,
     * then what it is doing under that. The list is short - a handful of radios in range - so
     * the rows are affordable here in a way they would not be on the node list, and the state
     * is the thing this screen exists to answer. It used to be one line with the state
     * concatenated onto the end of the name, where a long name pushed it off the panel.
     *
     * There is no empty state, and that is the Waypoints tab's rule: the last row is always the
     * network one, so a screen falling through to a picture would take away the only control
     * that can put a radio on this list at all. What stands where the radios would be instead
     * is one row past what the nav counts - the Nodes tab's "no nodes match this filter" line,
     * drawn the same way, and unreachable for the same reason: mesh_ui_devices_row_count() does
     * not know about it and the nav clamps to that. It goes *after* the network row rather than
     * before it so every real row keeps the index the nav gave it, or the highlight and the
     * press would be one row apart on exactly the screen with nothing else on it.
     */
    const uint32_t rows = mesh_ui_devices_row_count(snapshot->devices, snapshot->device_count);
    const bool nothing_found = (snapshot->device_count == 0U);
    struct inkcell_fb_list list = inkcell_fb_list_begin_rows(
        layout, rows + (nothing_found ? 1U : 0U), nav->cursor[MESH_UI_SCREEN_DEVICES], 2U);
    inkcell_fb_list_glide(state, &list, FB_LIST_DEVICES);
    inkcell_fb_list_focus(&list, (uint32_t)MESH_UI_FOCUS_ROWS);
    char attach[48]; /* "%ddBm at last scan", and room for a language longer than English */
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
        struct mesh_ui_devices_row entry;
        if (!mesh_ui_devices_row(snapshot->devices, snapshot->device_count, snapshot->network_host,
                                 i, &entry)) {
            /* The row that is not a row: what the client is still doing, where the radios
               would be. Dim because there is nothing here to press. */
            inkcell_fb_list_row(state, &list, i, inkcell_str(MESH_STR_DEVICES_EMPTY),
                                INKCELL_TONE_DIM);
            continue;
        }
        if (entry.type == (uint8_t)MESH_UI_DEVICES_ROW_NETWORK) {
            fb_devices_network_row(state, &list, i, &entry);
            continue;
        }
        const struct mesh_ui_device *device = entry.device;
        const char *name = device->name[0] != '\0' ? device->name : device->identifier;
        if (name[0] == '\0') {
            name = inkcell_str(MESH_STR_DEVICES_UNNAMED);
        }
        /*
         * What pressing A on this row would do. An unpaired BLE node is the case worth
         * calling out: it connects and then fails on StartNotify unless it is bonded first,
         * which is exactly what A now does for it.
         *
         * It is a capsule rather than a word, which is what the catalog ids have called it
         * since they were written. A device list is four rows about one question - which of
         * these am I on - so the state is the thing the eye is scanning for, and a state set
         * as prose on a supporting line is the one shape that cannot be scanned: every row
         * reads the same until it has been read. The family is the sentence: good for the one
         * we are on, the accent for a step in flight, warning for the one that will fail on
         * StartNotify until it is bonded.
         *
         * `paired` gets no capsule, and that is the whole of what the other three are worth.
         * It is the resting state of a bonded radio - every row in a list of known radios has
         * it - so a pill there is on every row at once, which is a column of colour reporting
         * nothing. Worse than nothing on two themes: the contrast palette has one yellow and
         * the colourblind palette one blue, so a resting capsule came out the same colour as
         * the warning beside it on the first and as `connected` on the second. A quiet word in
         * the same slot says the same thing and leaves the colour to the rows that have
         * something to report. Anything a badge does not shout is a badge that should not be
         * there.
         */
        const char *status = "";
        enum inkcell_family status_family = INKCELL_FAMILY_PRIMARY;
        bool status_badge = true;
        if (device->connected) {
            status = inkcell_str(MESH_STR_DEVICES_BADGE_CONNECTED);
            status_family = INKCELL_FAMILY_SUCCESS;
        } else if (device->busy) {
            status = inkcell_str(MESH_STR_DEVICES_BADGE_WORKING);
        } else if (device->bootloader) {
            /* Ahead of the BLE arms because it is the one refusal a USB row can carry, and
               warning for the same reason `needs pairing` is: the row will not connect as it
               stands, and there is something the user can do about it. */
            status = inkcell_str(MESH_STR_DEVICES_BADGE_BOOTLOADER);
            status_family = INKCELL_FAMILY_WARNING;
        } else if (device->kind == (uint8_t)MESH_UI_DEVICE_BLE && !device->paired) {
            status = inkcell_str(MESH_STR_DEVICES_BADGE_NEEDS_PAIR);
            status_family = INKCELL_FAMILY_WARNING;
        } else if (device->kind == (uint8_t)MESH_UI_DEVICE_BLE) {
            /* The radio a launch reconnects to says so in the same quiet slot: it is the answer
               to "why did it pick that one", and it is still a resting state, not news. */
            status = inkcell_str(device->preferred ? MESH_STR_DEVICES_BADGE_AUTO
                                                   : MESH_STR_DEVICES_BADGE_PAIRED);
            status_badge = false;
        }

        /* A USB port has no RSSI to show, so it says which bus it is instead - the supporting
           line answers "how is this attached" either way. */
        if (device->kind == (uint8_t)MESH_UI_DEVICE_SERIAL) {
            inkwell_str_copy(attach, sizeof attach, inkcell_str(MESH_STR_DEVICES_TRAILING_USB));
        } else if (device->kind == (uint8_t)MESH_UI_DEVICE_TCP) {
            /* Ahead of the in-range arm, which a network link would otherwise fall into and
               answer "not in range" - a sentence about earshot, said of the one link that has
               none to be outside of. */
            inkwell_str_copy(attach, sizeof attach, inkcell_str(MESH_STR_DEVICES_TRAILING_NETWORK));
        } else if (device->reading == (uint8_t)MESH_UI_READING_LIVE) {
            inkcell_str_format(attach, sizeof attach, MESH_STR_DEVICES_TRAILING_RSSI,
                               (int)device->rssi);
        } else if (device->reading == (uint8_t)MESH_UI_READING_LAST_SCAN) {
            /* The scan is held for a link and this is what it last measured - a reading, but
               one that has stopped moving, and the row says which. */
            inkcell_str_format(attach, sizeof attach, MESH_STR_DEVICES_TRAILING_RSSI_LAST,
                               (int)device->rssi);
        } else if (device->connected) {
            /* A Bluetooth link reports no signal once it is up. The 0 left in the struct drew as
               "0dBm", the strongest reading there is, about the one radio nothing measured. */
            inkwell_str_copy(attach, sizeof attach, inkcell_str(MESH_STR_DEVICES_TRAILING_BLE));
        } else if (device->reading == (uint8_t)MESH_UI_READING_SCAN_HELD) {
            /* Unheard by a scan that is not running is not out of range: it is unknown. */
            inkwell_str_copy(attach, sizeof attach, inkcell_str(MESH_STR_DEVICES_TRAILING_HELD));
        } else {
            /* A bond BlueZ holds for a radio it cannot hear has no reading behind it, and the
               0 that leaves in the struct would draw as the strongest node on the screen. */
            inkwell_str_copy(attach, sizeof attach, inkcell_str(MESH_STR_DEVICES_TRAILING_AWAY));
        }

        const bool armed = nav->devices_forget_armed && nav->devices_forget_row == i;
        enum inkcell_tone tone = INKCELL_TONE_NORMAL;
        if (device->connected) {
            tone = INKCELL_TONE_SUCCESS;
        } else if (armed) {
            tone = INKCELL_TONE_ERROR;
        }
        /* A row armed to be forgotten says so in every part of itself, the resting state
           included: the capsule reports the link, which is a different fact, but a red row
           carrying a green pill is two rows' worth of statement in one and the press being
           asked about is the destructive one. */
        if (armed) {
            status_family = INKCELL_FAMILY_ERROR;
            status_badge = true;
        }

        /* The disc states its fill rather than taking a tint: a device list is four rows about
           one question - which of these am I on - and six hues would be answering a question
           nobody asked. Connected is the good tone, armed to be forgotten is the bad one.
           What it carries is the transport, not initials: the name is already the next thing
           on the row, and which bus a radio is on is the one fact about it the words do not
           repeat. */
        const struct inkcell_fb_list_item row = {
            .leading =
                {
                    .kind = INKCELL_FB_LEADING_AVATAR,
                    .icon = fb_device_icon(device),
                    .tint = i,
                    .role = device->connected ? INKCELL_COLOR_SUCCESS
                            : armed           ? INKCELL_COLOR_ERROR
                                              : INKCELL_COLOR_COUNT,
                },
            .text = name,
            .tone = tone,
            /*
             * The two facts swap lines, and the order is the point. The capsule takes the
             * headline's trailing edge because how a radio is attached is a detail and whether
             * it is the one we are on is not; how well we hear it drops to the supporting line,
             * where it keeps the dim ink it already had.
             *
             * A row with nothing to report - a USB port that is merely present - draws no
             * capsule and loses nothing: the slot is right-aligned, so unlike the leading
             * gutter an empty one costs no column, and neither kind draws at all on an empty
             * string.
             */
            .trailing = status_badge
                            ? (struct inkcell_fb_trailing){.kind = INKCELL_FB_TRAILING_BADGE,
                                                           .family = status_family,
                                                           .text = status}
                            : (struct inkcell_fb_trailing){.kind = INKCELL_FB_TRAILING_TEXT,
                                                           .text = status},
            .supporting = attach,
            .supporting_tone = INKCELL_TONE_DIM,
            /* A figure is something the eye glances at on its way past, on the ground and under
               the cursor alike - which is what the trailing slot it used to sit in already did
               for it, and what it keeps here. */
            .supporting_quiet = true,
            .divider = true,
        };
        inkcell_fb_list_item(state, &list, i, &row);
    }
}
