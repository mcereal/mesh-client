#define _POSIX_C_SOURCE 200809L

/*
 * What became of a message we sent, as the UI says it.
 *
 * The table in src/ui/delivery.c is the one place that decides which mark an ack state gets,
 * and it exists because two places draw one: the transcript puts the icon in a bubble's corner
 * and a backend with no sprites has to say the word instead. The cases below hold the table to
 * three things - every state on the wire is answered, every mark it answers with is one this
 * build can actually draw, and no two states share a mark - because a table that gave two
 * states the same picture would be a bubble that cannot tell "still going" from "arrived", and
 * nothing in the renderer would notice.
 */

#include "framework/mesh_test.h"

#include "mesh/core/message.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/delivery.h"

#include <stdio.h>
#include <string.h>

/* Every state an outbound message can be in, which is the enum's whole range. */
static const enum mesh_message_ack kAcks[] = {
    MESH_MESSAGE_ACK_NONE,
    MESH_MESSAGE_ACK_PENDING,
    MESH_MESSAGE_ACK_DELIVERED,
    MESH_MESSAGE_ACK_FAILED,
};

MESH_TEST_CASE(ui_delivery_marks_every_state_it_can_be_asked_about, unit) {
    /* Nothing to wait for gets no mark at all. A broadcast goes out without want_ack, so this
       is the common case on a channel - and a tick there would be the client reporting a
       delivery no Routing reply ever confirmed. */
    const struct mesh_ui_delivery none = mesh_ui_delivery_of((uint8_t)MESH_MESSAGE_ACK_NONE);
    MESH_TEST_FAIL_IF(mesh_ui_icon_is_valid(none.icon),
                      "a message with nothing to wait for should carry no mark");

    for (size_t i = 1U; i < sizeof kAcks / sizeof kAcks[0]; ++i) {
        const struct mesh_ui_delivery mark = mesh_ui_delivery_of((uint8_t)kAcks[i]);
        char detail[96];
        snprintf(detail, sizeof detail, "state %u has no drawable mark", (unsigned)kAcks[i]);
        MESH_TEST_FAIL_IF(!mesh_ui_icon_is_valid(mark.icon), detail);
        /* A sprite this build can find, rather than an id past the end of the generated table -
           which is what an icons.def edit committed without rerunning gen-icons.py leaves. */
        snprintf(detail, sizeof detail, "state %u names a sprite this build cannot draw",
                 (unsigned)kAcks[i]);
        MESH_TEST_FAIL_IF(mesh_ui_icon_name(mark.icon)[0] == '\0', detail);
        snprintf(detail, sizeof detail, "state %u has no word", (unsigned)kAcks[i]);
        MESH_TEST_FAIL_IF(mesh_str(mark.word)[0] == '\0', detail);
    }
    record_success(test_name);
}

MESH_TEST_CASE(ui_delivery_marks_are_distinct, unit) {
    for (size_t i = 0; i < sizeof kAcks / sizeof kAcks[0]; ++i) {
        for (size_t j = i + 1U; j < sizeof kAcks / sizeof kAcks[0]; ++j) {
            const struct mesh_ui_delivery a = mesh_ui_delivery_of((uint8_t)kAcks[i]);
            const struct mesh_ui_delivery b = mesh_ui_delivery_of((uint8_t)kAcks[j]);
            char detail[96];
            snprintf(detail, sizeof detail, "states %u and %u draw the same mark",
                     (unsigned)kAcks[i], (unsigned)kAcks[j]);
            MESH_TEST_FAIL_IF(a.icon == b.icon, detail);
            snprintf(detail, sizeof detail, "states %u and %u say the same word",
                     (unsigned)kAcks[i], (unsigned)kAcks[j]);
            MESH_TEST_FAIL_IF(strcmp(mesh_str(a.word), mesh_str(b.word)) == 0, detail);
        }
    }
    record_success(test_name);
}

/*
 * A state the enum does not have gets nothing rather than something.
 *
 * The ack travels to the UI as a byte in the snapshot, and upstream's Routing_Error set grows;
 * a table that fell through to a mark would put a stray glyph in the corner of every bubble the
 * day a firmware started reporting a fourth state.
 */
MESH_TEST_CASE(ui_delivery_refuses_a_state_it_does_not_know, unit) {
    const struct mesh_ui_delivery mark = mesh_ui_delivery_of(200U);
    MESH_TEST_FAIL_IF(mesh_ui_icon_is_valid(mark.icon), "an unknown state should carry no mark");
    MESH_TEST_FAIL_IF(mesh_str(mark.word)[0] != '\0', "an unknown state should say nothing");
    record_success(test_name);
}
