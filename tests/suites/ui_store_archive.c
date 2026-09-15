#define _POSIX_C_SOURCE 200809L

/*
 * The per-conversation transcript on the card, and the deep window the thread screen draws from
 * it.
 *
 * What is held here is the claim the whole thing exists to make: what the client remembers is
 * no longer what the radio is holding. The flat list is 64 across every conversation at once
 * because the transport ring is; a conversation's file is its own, and a busy channel filling
 * one takes nothing from a quiet one.
 */

#include "framework/mesh_test.h"
#include "support/fs_fixture.h"

#include "mesh/core/message.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"
#include "mesh/ui/store_archive.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- fixtures ------------------------------------------------------------------------------ */

/* A directory of this suite's own, taken down by the case that made it. */
static bool archive_open(struct mesh_ui_archive *archive, char *dir, size_t dir_len) {
    snprintf(dir, dir_len, "/tmp/mesh_archive_XXXXXX");
    if (mkdtemp(dir) == NULL) {
        return false;
    }
    /* mkdtemp has already made it; the archive is pointed at it and finds it there. */
    return mesh_ui_archive_init(archive, dir) == 0;
}

static void archive_close(struct mesh_ui_archive *archive, const char *dir) {
    (void)archive;
    (void)mesh_test_remove_tree(dir);
}

/* One message in the shape a publish hands the archive. */
static struct mesh_ui_message archive_message(uint32_t packet_id, bool broadcast, uint32_t peer,
                                              uint8_t channel, const char *text) {
    struct mesh_ui_message message;
    memset(&message, 0, sizeof message);
    message.packet_id = packet_id;
    message.peer = peer;
    message.channel = channel;
    message.broadcast = broadcast;
    message.rx_time = 1000U + packet_id;
    snprintf(message.text, sizeof message.text, "%s", text);
    snprintf(message.peer_name, sizeof message.peer_name, "N%03u", (unsigned)(peer & 0xFFFU));
    return message;
}

static void archive_list_of(struct mesh_ui_message_list *list, const struct mesh_ui_message *items,
                            uint32_t count) {
    memset(list, 0, sizeof *list);
    for (uint32_t i = 0; i < count && i < MESH_UI_MAX_MESSAGES; ++i) {
        list->entries[list->count++] = items[i];
    }
}

/* ---- the format round trip ------------------------------------------------------------------ */

/*
 * A message written to the card comes back the same message, and lands in its own conversation.
 *
 * The second half is the part worth a test rather than a glance: the file is chosen by a
 * (kind, node, channel) that the append derives from the message itself, and a broadcast on
 * channel 0 and a direct exchange with node 0 are exactly the pair a single name space would
 * have collided.
 */
MESH_TEST_CASE(ui_archive_round_trip, unit) {
    const char *failure = NULL;
    struct mesh_ui_archive archive;
    char dir[64];
    MESH_TEST_FAIL_IF(!archive_open(&archive, dir, sizeof dir), "could not open an archive");

    const struct mesh_ui_message items[] = {
        archive_message(11U, true, 0U, 0U, "on the channel"),
        archive_message(12U, false, 0x3000U, 0U, "to BRVO"),
        archive_message(13U, true, 0U, 0U, "channel again: a=b\\c"),
    };
    struct mesh_ui_message_list list;
    archive_list_of(&list, items, 3U);

    if (mesh_ui_archive_append(&archive, &list) != 3) {
        failure = "all three messages should have been written";
        goto cleanup;
    }

    struct mesh_ui_thread window;
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_CHANNEL, 0U, 0U,
                                    &window) != 0) {
        failure = "loading the channel's transcript failed";
        goto cleanup;
    }
    if (!window.valid || window.count != 2U || window.dropped != 0U) {
        failure = "the channel should hold its two messages and nothing else";
        goto cleanup;
    }
    if (window.entries[0].packet_id != 11U || window.entries[1].packet_id != 13U) {
        failure = "the channel's messages should come back oldest first";
        goto cleanup;
    }
    /* Through the escape and back: '=' and '\\' are exactly what would otherwise forge a line. */
    if (strcmp(window.entries[1].text, "channel again: a=b\\c") != 0) {
        failure = "a message carrying an '=' and a backslash should survive the trip";
        goto cleanup;
    }
    if (!window.entries[0].broadcast || strcmp(window.entries[0].peer_name, "N000") != 0) {
        failure = "a record should come back with what it is, not only what it said";
        goto cleanup;
    }

    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x3000U, 0U,
                                    &window) != 0) {
        failure = "loading the direct transcript failed";
        goto cleanup;
    }
    if (window.count != 1U || window.entries[0].packet_id != 12U) {
        failure = "the direct conversation should hold only its own message";
        goto cleanup;
    }

    /* A conversation nobody has said anything in is a valid empty window rather than an error,
       so the thread screen draws an empty conversation rather than falling back to the log. */
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x9999U, 0U,
                                    &window) != 0 ||
        !window.valid || window.count != 0U) {
        failure = "an untouched conversation should load as a valid empty window";
        goto cleanup;
    }

cleanup:
    archive_close(&archive, dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The point of the whole thing: one conversation holds more than the flat list can.
 *
 * MESH_UI_MAX_MESSAGES is the transport ring and it is shared by every conversation at once. A
 * hundred messages in one conversation is under two days on a moderately busy channel and used
 * to be the end of the transcript; here they are appended a publish at a time, exactly as a
 * running client would, and every one of them comes back.
 */
MESH_TEST_CASE(ui_archive_outlives_the_flat_list, unit) {
    const char *failure = NULL;
    struct mesh_ui_archive archive;
    char dir[64];
    MESH_TEST_FAIL_IF(!archive_open(&archive, dir, sizeof dir), "could not open an archive");

    const uint32_t total = MESH_UI_MAX_MESSAGES * 2U;
    for (uint32_t i = 0; i < total; ++i) {
        char text[32];
        snprintf(text, sizeof text, "message %u", (unsigned)i);
        const struct mesh_ui_message one = archive_message(i + 1U, true, 0U, 3U, text);
        struct mesh_ui_message_list list;
        archive_list_of(&list, &one, 1U);
        if (mesh_ui_archive_append(&archive, &list) != 1) {
            failure = "each publish should append its one new message";
            goto cleanup;
        }
    }

    struct mesh_ui_thread window;
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_CHANNEL, 0U, 3U,
                                    &window) != 0) {
        failure = "loading the transcript failed";
        goto cleanup;
    }
    if (window.count != total) {
        failure = "the card should hold more than the flat list ever could";
        goto cleanup;
    }
    if (window.entries[0].packet_id != 1U || window.entries[total - 1U].packet_id != total) {
        failure = "the transcript should read oldest first across every publish";
        goto cleanup;
    }

cleanup:
    archive_close(&archive, dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A message sitting in the transport ring across several publishes is written once.
 *
 * The ring holds a message for as long as it holds it, so the same list arrives at the archive
 * on every publish that carries traffic. Without the recent-id check the transcript would grow
 * by the whole of the ring every couple of seconds.
 */
MESH_TEST_CASE(ui_archive_appends_a_message_once, unit) {
    const char *failure = NULL;
    struct mesh_ui_archive archive;
    char dir[64];
    MESH_TEST_FAIL_IF(!archive_open(&archive, dir, sizeof dir), "could not open an archive");

    const struct mesh_ui_message items[] = {
        archive_message(41U, true, 0U, 0U, "first"),
        archive_message(42U, true, 0U, 0U, "second"),
    };
    struct mesh_ui_message_list list;
    archive_list_of(&list, items, 2U);

    if (mesh_ui_archive_append(&archive, &list) != 2) {
        failure = "the first publish should write both";
        goto cleanup;
    }
    if (mesh_ui_archive_append(&archive, &list) != 0) {
        failure = "the same ring published again should write nothing";
        goto cleanup;
    }

    /* A third message arrives and the older two are still in the ring beside it. */
    struct mesh_ui_message grown[3] = {items[0], items[1],
                                       archive_message(43U, true, 0U, 0U, "third")};
    archive_list_of(&list, grown, 3U);
    if (mesh_ui_archive_append(&archive, &list) != 1) {
        failure = "only the message that is actually new should be written";
        goto cleanup;
    }

    struct mesh_ui_thread window;
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_CHANNEL, 0U, 0U,
                                    &window) != 0 ||
        window.count != 3U) {
        failure = "the transcript should hold each message exactly once";
        goto cleanup;
    }

    /* A message with no packet id names nothing and is declined rather than written on every
       publish for as long as the ring holds it. */
    const struct mesh_ui_message anonymous = archive_message(0U, true, 0U, 0U, "no id");
    archive_list_of(&list, &anonymous, 1U);
    if (mesh_ui_archive_append(&archive, &list) != 0) {
        failure = "a message the radio gave no id has nothing to deduplicate against";
        goto cleanup;
    }

cleanup:
    archive_close(&archive, dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A file longer than the window reads as its newest end, and says how much is behind it.
 *
 * The reader folds a whole file through a ring the size of the window, so this is the check
 * that the ring unwinds into transcript order rather than into the order the slots happen to
 * be in - the failure it guards is a thread whose messages are right and whose *sequence* is
 * cut and swapped at an arbitrary point.
 */
MESH_TEST_CASE(ui_archive_window_keeps_the_newest_end, unit) {
    const char *failure = NULL;
    struct mesh_ui_archive archive;
    char dir[64];
    MESH_TEST_FAIL_IF(!archive_open(&archive, dir, sizeof dir), "could not open an archive");

    const uint32_t extra = 10U;
    const uint32_t total = MESH_UI_MAX_THREAD_MESSAGES + extra;
    for (uint32_t i = 0; i < total; ++i) {
        const struct mesh_ui_message one = archive_message(i + 1U, false, 0x4242U, 0U, "x");
        struct mesh_ui_message_list list;
        archive_list_of(&list, &one, 1U);
        if (mesh_ui_archive_append(&archive, &list) != 1) {
            failure = "append failed part way through";
            goto cleanup;
        }
    }

    struct mesh_ui_thread window;
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x4242U, 0U,
                                    &window) != 0) {
        failure = "loading the transcript failed";
        goto cleanup;
    }
    if (window.count != MESH_UI_MAX_THREAD_MESSAGES) {
        failure = "the window should fill and stop";
        goto cleanup;
    }
    if (window.dropped != extra) {
        failure = "the window should count exactly what it had no room for";
        goto cleanup;
    }
    /* The whole run in order, which is what the ring's unwinding is for. */
    for (uint32_t i = 0; i < window.count; ++i) {
        if (window.entries[i].packet_id != extra + i + 1U) {
            failure = "the window should be the newest end of the file, in order";
            goto cleanup;
        }
    }

cleanup:
    archive_close(&archive, dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Seeding writes for a conversation with no file and leaves an existing one alone.
 *
 * This is the upgrade path, run on every start: a card from a build with no archive has its
 * history in the handshake cache and nowhere else. Running it against a conversation that
 * already has a file would replace a real transcript with the 64 entries the cache holds -
 * which is the one way this could destroy the thing it exists to preserve.
 */
MESH_TEST_CASE(ui_archive_seeds_only_what_is_missing, unit) {
    const char *failure = NULL;
    struct mesh_ui_archive archive;
    char dir[64];
    MESH_TEST_FAIL_IF(!archive_open(&archive, dir, sizeof dir), "could not open an archive");

    /* A conversation with a real transcript behind it. */
    for (uint32_t i = 0; i < 5U; ++i) {
        const struct mesh_ui_message one = archive_message(100U + i, true, 0U, 1U, "history");
        struct mesh_ui_message_list list;
        archive_list_of(&list, &one, 1U);
        (void)mesh_ui_archive_append(&archive, &list);
    }

    /* The cache's restored history: one message for that conversation, one for a fresh one. */
    const struct mesh_ui_message restored[] = {
        archive_message(100U, true, 0U, 1U, "history"),
        archive_message(200U, false, 0x7777U, 0U, "from the cache"),
    };
    struct mesh_ui_message_list cached;
    archive_list_of(&cached, restored, 2U);

    if (mesh_ui_archive_seed(&archive, &cached) != 1) {
        failure = "only the conversation without a file should be seeded";
        goto cleanup;
    }

    struct mesh_ui_thread window;
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_CHANNEL, 0U, 1U,
                                    &window) != 0 ||
        window.count != 5U) {
        failure = "a conversation that already had a transcript should be untouched";
        goto cleanup;
    }
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x7777U, 0U,
                                    &window) != 0 ||
        window.count != 1U || window.entries[0].packet_id != 200U) {
        failure = "the conversation with no file should have been given the cache's history";
        goto cleanup;
    }

    /* And a seeded record is not then appended again by the publish that follows. */
    if (mesh_ui_archive_append(&archive, &cached) != 0) {
        failure = "a seeded message is already on the card";
        goto cleanup;
    }

cleanup:
    archive_close(&archive, dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Deleting reaches the card, both by conversation and by message - and a deleted message takes
 * its reactions with it.
 *
 * The card's copy is the one that outlives a restart, so a delete that missed it would be a
 * message the reader threw away that came back the next time they opened the thread.
 */
MESH_TEST_CASE(ui_archive_delete_reaches_the_card, unit) {
    const char *failure = NULL;
    struct mesh_ui_archive archive;
    char dir[64];
    MESH_TEST_FAIL_IF(!archive_open(&archive, dir, sizeof dir), "could not open an archive");

    struct mesh_ui_message items[4] = {
        archive_message(51U, false, 0x5000U, 0U, "keep me"),
        archive_message(52U, false, 0x5000U, 0U, "delete me"),
        archive_message(53U, false, 0x5000U, 0U, "\xF0\x9F\x91\x8D"),
        archive_message(54U, false, 0x5000U, 0U, "keep me too"),
    };
    items[2].is_reaction = true;
    items[2].reply_id = 52U;

    struct mesh_ui_message_list list;
    archive_list_of(&list, items, 4U);
    if (mesh_ui_archive_append(&archive, &list) != 4) {
        failure = "the fixture should have been written";
        goto cleanup;
    }

    /* The message and the tapback on it: two records for one press. */
    if (mesh_ui_archive_forget_message(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x5000U, 0U,
                                       52U) != 2) {
        failure = "a deleted message should take the reactions about it";
        goto cleanup;
    }

    struct mesh_ui_thread window;
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x5000U, 0U,
                                    &window) != 0) {
        failure = "loading after a delete failed";
        goto cleanup;
    }
    if (window.count != 2U || window.entries[0].packet_id != 51U ||
        window.entries[1].packet_id != 54U) {
        failure = "the rest of the conversation should be exactly as it was";
        goto cleanup;
    }

    /* Deleting it again finds nothing, and says so rather than failing. */
    if (mesh_ui_archive_forget_message(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x5000U, 0U,
                                       52U) != 0) {
        failure = "a message already gone should remove nothing";
        goto cleanup;
    }

    if (mesh_ui_archive_forget_conversation(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x5000U,
                                            0U) != 0) {
        failure = "removing the conversation's file failed";
        goto cleanup;
    }
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x5000U, 0U,
                                    &window) != 0 ||
        window.count != 0U) {
        failure = "a deleted conversation should have nothing left on the card";
        goto cleanup;
    }
    /* Twice is not a failure: the press can race the file already being gone. */
    if (mesh_ui_archive_forget_conversation(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x5000U,
                                            0U) != 0) {
        failure = "removing a conversation that has no file is not an error";
        goto cleanup;
    }

cleanup:
    archive_close(&archive, dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* An archive with nowhere to write does nothing and says nothing went wrong, so a client on a
   full or read-only card still runs. */
MESH_TEST_CASE(ui_archive_without_a_directory_is_quiet, unit) {
    struct mesh_ui_archive archive;
    MESH_TEST_FAIL_IF(mesh_ui_archive_init(&archive, "") == 0,
                      "an empty directory should not open an archive");

    const struct mesh_ui_message one = archive_message(1U, true, 0U, 0U, "nowhere to go");
    struct mesh_ui_message_list list;
    archive_list_of(&list, &one, 1U);

    MESH_TEST_FAIL_IF(mesh_ui_archive_append(&archive, &list) != 0,
                      "a disabled archive should write nothing and report no failure");
    MESH_TEST_FAIL_IF(mesh_ui_archive_seed(&archive, &list) != 0,
                      "a disabled archive should seed nothing and report no failure");

    struct mesh_ui_thread window;
    MESH_TEST_FAIL_IF(mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_CHANNEL,
                                                  0U, 0U, &window) != 0 ||
                          window.valid || window.count != 0U,
                      "a disabled archive should yield a window the thread screen falls back from");
    record_success(test_name);
}

/* ---- the window the screen draws ------------------------------------------------------------ */

/*
 * The live log folded onto a window read off the card.
 *
 * Three behaviours, and the first is the one that is easy to get wrong: a message already in the
 * window is updated *where it sits*, so a bubble does not leap to the bottom of the transcript
 * the moment its delivery mark arrives.
 */
MESH_TEST_CASE(ui_thread_merge_folds_the_live_log_in, unit) {
    const char *failure = NULL;

    struct mesh_ui_thread window;
    memset(&window, 0, sizeof window);
    window.kind = (uint8_t)MESH_UI_CONVERSATION_DIRECT;
    window.node = 0x6000U;
    window.valid = true;
    window.entries[0] = archive_message(61U, false, 0x6000U, 0U, "from the card");
    window.entries[1] = archive_message(62U, false, 0x6000U, 0U, "also from the card");
    window.count = 2U;

    struct mesh_ui_message live[3] = {
        archive_message(61U, false, 0x6000U, 0U, "from the card"),
        archive_message(63U, false, 0x6000U, 0U, "new this session"),
        archive_message(64U, true, 0U, 0U, "somebody else's channel"),
    };
    live[0].ack = MESH_MESSAGE_ACK_DELIVERED;

    struct mesh_ui_message_list list;
    archive_list_of(&list, live, 3U);
    mesh_ui_thread_merge(&window, &list);

    if (window.count != 3U) {
        failure = "the new message should be appended and the known one folded in";
        goto done;
    }
    if (window.entries[0].packet_id != 61U || window.entries[0].ack != MESH_MESSAGE_ACK_DELIVERED) {
        failure = "a message already in the window should be updated where it sits";
        goto done;
    }
    if (window.entries[2].packet_id != 63U) {
        failure = "a message this session heard should land at the end";
        goto done;
    }
    for (uint32_t i = 0; i < window.count; ++i) {
        if (window.entries[i].packet_id == 64U) {
            failure = "a message from another conversation has no business in this window";
            goto done;
        }
    }

done:
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A full window evicts its oldest and counts it, so the transcript keeps saying how much is
   behind the top of it rather than quietly losing the number. */
MESH_TEST_CASE(ui_thread_merge_evicts_the_oldest, unit) {
    const char *failure = NULL;

    struct mesh_ui_thread window;
    memset(&window, 0, sizeof window);
    window.kind = (uint8_t)MESH_UI_CONVERSATION_CHANNEL;
    window.channel = 2U;
    window.valid = true;
    for (uint32_t i = 0; i < MESH_UI_MAX_THREAD_MESSAGES; ++i) {
        window.entries[i] = archive_message(i + 1U, true, 0U, 2U, "old");
    }
    window.count = MESH_UI_MAX_THREAD_MESSAGES;

    const struct mesh_ui_message fresh = archive_message(9001U, true, 0U, 2U, "new");
    struct mesh_ui_message_list list;
    archive_list_of(&list, &fresh, 1U);
    mesh_ui_thread_merge(&window, &list);

    if (window.count != MESH_UI_MAX_THREAD_MESSAGES || window.dropped != 1U) {
        failure = "a full window should stay full and count what it let go";
        goto done;
    }
    if (window.entries[0].packet_id != 2U ||
        window.entries[MESH_UI_MAX_THREAD_MESSAGES - 1U].packet_id != 9001U) {
        failure = "the oldest should go and the newest should arrive at the end";
        goto done;
    }

done:
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The view is the window only while the window is over the conversation the nav has open.
 *
 * This is the guard against the one failure a transcript must not have: somebody else's
 * messages drawn under the right title. The nav turns on a press and the window is refilled a
 * publish later, so for one frame after opening a thread the window still holds the
 * conversation the reader just left.
 */
MESH_TEST_CASE(ui_store_message_view_picks_the_window, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    const struct mesh_ui_message flat = archive_message(71U, false, 0x7000U, 0U, "in the log");
    struct mesh_ui_message_list list;
    archive_list_of(&list, &flat, 1U);
    mesh_ui_store_set_messages(&store, &list);

    struct mesh_ui_thread window;
    memset(&window, 0, sizeof window);
    window.kind = (uint8_t)MESH_UI_CONVERSATION_DIRECT;
    window.node = 0x7000U;
    window.valid = true;
    window.entries[0] = archive_message(70U, false, 0x7000U, 0U, "off the card");
    window.entries[1] = flat;
    window.count = 2U;
    mesh_ui_store_set_thread(&store, &window);

    /* The conversation list: the flat list, because a list of conversations is derived from
       every conversation at once. */
    struct mesh_ui_message_view view = mesh_ui_store_message_view(&store, &store.nav);
    if (view.count != 1U) {
        failure = "with no thread open the view should be the flat list";
        goto cleanup;
    }

    /* That conversation open: the window, which is deeper. */
    store.nav.thread_open = true;
    store.nav.inbox = false;
    store.nav.target_node = 0x7000U;
    view = mesh_ui_store_message_view(&store, &store.nav);
    if (view.count != 2U || view.entries[0].packet_id != 70U) {
        failure = "the open conversation should be drawn from its window";
        goto cleanup;
    }

    /* A different conversation open, window not yet refilled: the flat list, not the window. */
    store.nav.target_node = 0x8000U;
    view = mesh_ui_store_message_view(&store, &store.nav);
    if (view.count != 1U) {
        failure = "a window over another conversation must never be drawn under this title";
        goto cleanup;
    }

    /* All traffic is several conversations at once and has no window by definition. */
    store.nav.target_node = 0x7000U;
    store.nav.inbox = true;
    view = mesh_ui_store_message_view(&store, &store.nav);
    if (view.count != 1U) {
        failure = "the all-traffic view should be the flat list";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* ---- deleting one message ------------------------------------------------------------------- */

/* The store's two copies of a transcript both lose the message, and the reactions about it go
   with it. The read mark stays, unlike a conversation delete: one message going does not un-see
   the rest. */
MESH_TEST_CASE(ui_store_forget_message, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_message items[3] = {
        archive_message(81U, false, 0x8100U, 0U, "keep"),
        archive_message(82U, false, 0x8100U, 0U, "go"),
        archive_message(83U, false, 0x8100U, 0U, "\xF0\x9F\x91\x8D"),
    };
    items[2].is_reaction = true;
    items[2].reply_id = 82U;

    struct mesh_ui_message_list list;
    archive_list_of(&list, items, 3U);
    mesh_ui_store_set_messages(&store, &list);

    struct mesh_ui_thread window;
    memset(&window, 0, sizeof window);
    window.kind = (uint8_t)MESH_UI_CONVERSATION_DIRECT;
    window.node = 0x8100U;
    window.valid = true;
    for (uint32_t i = 0; i < 3U; ++i) {
        window.entries[i] = items[i];
    }
    window.count = 3U;
    mesh_ui_store_set_thread(&store, &window);

    if (mesh_ui_store_forget_message(&store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x8100U, 0U,
                                     82U) != 2U) {
        failure = "the message and its tapback should both go";
        goto cleanup;
    }
    if (store.messages.count != 1U || store.messages.entries[0].packet_id != 81U) {
        failure = "the flat list should keep the rest of the conversation";
        goto cleanup;
    }
    if (store.thread.count != 1U || store.thread.entries[0].packet_id != 81U) {
        failure = "the window under the reader's eyes should lose it too";
        goto cleanup;
    }

    /* Upstream's "no id" names no message, so it removes nothing rather than everything. */
    if (mesh_ui_store_forget_message(&store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x8100U, 0U,
                                     0U) != 0U ||
        store.messages.count != 1U) {
        failure = "a packet id of 0 should delete nothing at all";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* The same on the transport's ring, which is the copy that would otherwise rebuild the store
   from underneath the delete on the very next publish. */
MESH_TEST_CASE(message_log_forget_message, unit) {
    const char *failure = NULL;

    struct mesh_message_log log;
    memset(&log, 0, sizeof log);
    for (uint32_t i = 0; i < 3U; ++i) {
        struct mesh_message *entry = &log.entries[log.count++];
        entry->packet_id = 91U + i;
        entry->from = 0x9100U;
        entry->to = 0x1U;
    }
    log.entries[2].is_reaction = true;
    log.entries[2].reply_id = 92U;
    log.dropped = 4U;

    if (mesh_message_log_forget_message(&log, 0x1U, 0U, 92U) != 2U) {
        failure = "the entry and the reaction naming it should both go";
        goto done;
    }
    if (log.count != 1U || log.entries[0].packet_id != 91U) {
        failure = "the rest of the ring should be untouched";
        goto done;
    }
    /* What the ring took away is not what the user took away, so the counter does not move. */
    if (log.dropped != 4U) {
        failure = "a delete is not an eviction and should not be counted as one";
        goto done;
    }
    if (mesh_message_log_forget_message(&log, 0x1U, 0U, 0U) != 0U || log.count != 1U) {
        failure = "a packet id of 0 names no entry";
        goto done;
    }

done:
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A file that outgrows its byte budget is cut back to the cap, not to the window.
 *
 * The distinction is the whole of what compaction is for, and it is easy to get wrong in the
 * cheap direction: the reader's natural buffer is the thread window, and compacting through one
 * would quietly make every conversation's history stop at whatever fitted on screen. So this
 * asserts on the number of records the *file* holds afterwards, not on what the window shows.
 */
MESH_TEST_CASE(ui_archive_compacts_to_the_cap, unit) {
    const char *failure = NULL;
    struct mesh_ui_archive archive;
    char dir[64];
    MESH_TEST_FAIL_IF(!archive_open(&archive, dir, sizeof dir), "could not open an archive");

    /* Long enough that the byte budget is reached in a few thousand records rather than tens of
       thousands; the format's own limit is MESH_UI_MESSAGE_TEXT_MAX. */
    char text[201];
    memset(text, 'x', sizeof text - 1U);
    text[sizeof text - 1U] = '\0';

    /* Published in ring-sized batches, as a busy radio would deliver them. */
    const uint32_t total = 4000U;
    uint32_t next_id = 1U;
    while (next_id <= total) {
        struct mesh_ui_message batch[MESH_UI_MAX_MESSAGES];
        uint32_t n = 0U;
        while (n < MESH_UI_MAX_MESSAGES && next_id <= total) {
            batch[n++] = archive_message(next_id++, true, 0U, 5U, text);
        }
        struct mesh_ui_message_list list;
        archive_list_of(&list, batch, n);
        if (mesh_ui_archive_append(&archive, &list) != (int)n) {
            failure = "every message in the batch should have been written";
            goto cleanup;
        }
    }

    /* Count the records on disk: a msg[] line is the one that opens one. */
    char path[128];
    snprintf(path, sizeof path, "%s/c05.log", dir);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        failure = "the conversation's file should exist";
        goto cleanup;
    }
    uint32_t records = 0U;
    char line[1400];
    while (fgets(line, sizeof line, file) != NULL) {
        if (strncmp(line, "msg[", 4) == 0) {
            records++;
        }
    }
    fclose(file);

    if (records >= total) {
        failure = "the file should have been compacted rather than grown without bound";
        goto cleanup;
    }
    if (records < MESH_UI_ARCHIVE_MAX_MESSAGES) {
        failure = "compaction should keep the cap, not cut the transcript back to the window";
        goto cleanup;
    }

    struct mesh_ui_thread window;
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_CHANNEL, 0U, 5U,
                                    &window) != 0) {
        failure = "loading a compacted transcript failed";
        goto cleanup;
    }
    if (window.count != MESH_UI_MAX_THREAD_MESSAGES) {
        failure = "the window should still fill from a compacted file";
        goto cleanup;
    }
    /* The newest message is the newest message: compaction keeps the end of the file. */
    if (window.entries[window.count - 1U].packet_id != total) {
        failure = "the last thing said should survive a compaction";
        goto cleanup;
    }
    if (window.dropped != records - MESH_UI_MAX_THREAD_MESSAGES) {
        failure = "the window should count exactly what the file held behind it";
        goto cleanup;
    }

cleanup:
    archive_close(&archive, dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Deleting one message out of a long transcript keeps the rest of it.
 *
 * The file is capped by bytes and ordinarily holds thousands of records, so a delete that read
 * it into a buffer and wrote the buffer back would discard everything that did not fit - which
 * is the transcript this whole feature exists to keep, thrown away by the press that was only
 * meant to remove one line. The delete streams instead; this is what holds it to that.
 */
MESH_TEST_CASE(ui_archive_delete_keeps_the_rest_of_a_long_file, unit) {
    const char *failure = NULL;
    struct mesh_ui_archive archive;
    char dir[64];
    MESH_TEST_FAIL_IF(!archive_open(&archive, dir, sizeof dir), "could not open an archive");

    /* Comfortably more than the delete could ever hold in one buffer. */
    const uint32_t total = MESH_UI_ARCHIVE_MAX_MESSAGES * 2U;
    uint32_t next_id = 1U;
    while (next_id <= total) {
        struct mesh_ui_message batch[MESH_UI_MAX_MESSAGES];
        uint32_t n = 0U;
        while (n < MESH_UI_MAX_MESSAGES && next_id <= total) {
            batch[n++] = archive_message(next_id++, false, 0xABCDU, 0U, "in the record");
        }
        struct mesh_ui_message_list list;
        archive_list_of(&list, batch, n);
        if (mesh_ui_archive_append(&archive, &list) != (int)n) {
            failure = "the fixture should have been written";
            goto cleanup;
        }
    }

    /* A message near the *newest* end, so a buffered rewrite would have found it and still
       thrown the older half away. */
    if (mesh_ui_archive_forget_message(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0xABCDU, 0U,
                                       total) != 1) {
        failure = "the message should have been deleted";
        goto cleanup;
    }

    char path[128];
    snprintf(path, sizeof path, "%s/n0000abcd.log", dir);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        failure = "the conversation's file should still be there";
        goto cleanup;
    }
    uint32_t records = 0U;
    bool found_oldest = false;
    bool found_target = false;
    char line[1400];
    while (fgets(line, sizeof line, file) != NULL) {
        if (strncmp(line, "msg[", 4) != 0) {
            continue;
        }
        records++;
        /* The packet id is the first field of the msg[] value. */
        const char *value = strchr(line, '=');
        if (value == NULL) {
            continue;
        }
        const unsigned long id = strtoul(value + 1, NULL, 10);
        if (id == 1UL) {
            found_oldest = true;
        }
        if (id == (unsigned long)total) {
            found_target = true;
        }
    }
    fclose(file);

    if (found_target) {
        failure = "the deleted message should be gone from the card";
        goto cleanup;
    }
    if (records != total - 1U) {
        failure = "a delete should remove one record and leave every other one alone";
        goto cleanup;
    }
    if (!found_oldest) {
        failure = "the oldest message must survive a delete at the other end of the file";
        goto cleanup;
    }

cleanup:
    archive_close(&archive, dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Two senders on one channel that land on the same packet id are two messages.
 *
 * MeshPacket.id only has to be unique per sender for a few minutes, and a channel's file holds
 * every sender on it - so an identity of "the packet id" would have the reader fold one node's
 * message onto another's and lose it. The sender is part of the identity for exactly this.
 */
MESH_TEST_CASE(ui_archive_tells_two_senders_apart, unit) {
    const char *failure = NULL;
    struct mesh_ui_archive archive;
    char dir[64];
    MESH_TEST_FAIL_IF(!archive_open(&archive, dir, sizeof dir), "could not open an archive");

    /* One id, two nodes, one channel - which upstream permits. */
    struct mesh_ui_message clash[2] = {
        archive_message(7777U, true, 0x1111U, 4U, "from ALFA"),
        archive_message(7777U, true, 0x2222U, 4U, "from BRVO"),
    };
    struct mesh_ui_message_list list;
    archive_list_of(&list, clash, 2U);

    if (mesh_ui_archive_append(&archive, &list) != 2) {
        failure = "both messages should be written: they are not the same message";
        goto cleanup;
    }

    struct mesh_ui_thread window;
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_CHANNEL, 0U, 4U,
                                    &window) != 0) {
        failure = "loading the channel failed";
        goto cleanup;
    }
    if (window.count != 2U) {
        failure = "two senders sharing a packet id are two messages, not one";
        goto cleanup;
    }
    if (strcmp(window.entries[0].text, "from ALFA") != 0 ||
        strcmp(window.entries[1].text, "from BRVO") != 0) {
        failure = "neither message should have been folded onto the other";
        goto cleanup;
    }

cleanup:
    archive_close(&archive, dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A message whose delivery state moves is written again, and the newer state is what loads.
 *
 * An outbound message is published pending and acknowledged a few seconds later. If the archive
 * treated "same packet id" as "already written", the card would keep the pending copy for good:
 * after a restart, a message that had in fact failed would read as still in flight, and the
 * transcript offers no resend on anything but a FAILED one.
 */
MESH_TEST_CASE(ui_archive_follows_a_delivery_state, unit) {
    const char *failure = NULL;
    struct mesh_ui_archive archive;
    char dir[64];
    MESH_TEST_FAIL_IF(!archive_open(&archive, dir, sizeof dir), "could not open an archive");

    struct mesh_ui_message sent = archive_message(300U, false, 0xBEEFU, 0U, "are you there?");
    sent.direction = MESH_MESSAGE_OUTBOUND;
    sent.ack = MESH_MESSAGE_ACK_PENDING;

    struct mesh_ui_message_list list;
    archive_list_of(&list, &sent, 1U);
    if (mesh_ui_archive_append(&archive, &list) != 1) {
        failure = "the outbound message should have been written";
        goto cleanup;
    }

    /* Published again, unchanged, as the ring keeps handing it over: written once. */
    if (mesh_ui_archive_append(&archive, &list) != 0) {
        failure = "an unchanged message should not be written twice";
        goto cleanup;
    }

    /* Now the mesh answers. */
    sent.ack = MESH_MESSAGE_ACK_FAILED;
    sent.ack_error = 5U;
    archive_list_of(&list, &sent, 1U);
    if (mesh_ui_archive_append(&archive, &list) != 1) {
        failure = "a message whose delivery state moved is worth writing again";
        goto cleanup;
    }

    struct mesh_ui_thread window;
    if (mesh_ui_archive_load_thread(&archive, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0xBEEFU, 0U,
                                    &window) != 0) {
        failure = "loading the transcript failed";
        goto cleanup;
    }
    if (window.count != 1U) {
        failure = "the two records are one message and should fold back into one bubble";
        goto cleanup;
    }
    if (window.entries[0].ack != MESH_MESSAGE_ACK_FAILED) {
        failure = "the later delivery state is the one that should survive a restart";
        goto cleanup;
    }
    /*
     * `ack_error` is deliberately not asserted: neither format on the card carries it - msg[]
     * writes the ack and not the reason behind it, in the archive and in the handshake cache
     * alike - so a failure restored from either reads as failed with the generic word rather
     * than with its Routing error. That is what fb_thread_row_build() falls back to, and it
     * predates this file; what matters here is that the bubble reads as failed at all, because
     * the resend the transcript offers is gated on exactly that.
     */

cleanup:
    archive_close(&archive, dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Deleting a bubble does not reach into another conversation that shares its packet id.
 *
 * The press names a conversation and the archive is scoped by opening one file; the three copies
 * in RAM have to be told, or one press would silently remove somebody else's message and the
 * next cache save would write that absence to the card.
 */
MESH_TEST_CASE(ui_store_forget_message_stays_in_its_conversation, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    /* The same id in two conversations, which upstream permits. */
    struct mesh_ui_message items[2] = {
        archive_message(555U, false, 0xAAAAU, 0U, "the one pressed on"),
        archive_message(555U, false, 0xBBBBU, 0U, "somebody else's"),
    };
    struct mesh_ui_message_list list;
    archive_list_of(&list, items, 2U);
    mesh_ui_store_set_messages(&store, &list);

    if (mesh_ui_store_forget_message(&store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0xAAAAU, 0U,
                                     555U) != 1U) {
        failure = "only the message in the conversation pressed on should go";
        goto cleanup;
    }
    if (store.messages.count != 1U || store.messages.entries[0].peer != 0xBBBBU) {
        failure = "the other conversation's message must survive";
        goto cleanup;
    }

    /* And the transport ring behaves the same way. */
    struct mesh_message log_entries;
    memset(&log_entries, 0, sizeof log_entries);
    struct mesh_message_log log;
    memset(&log, 0, sizeof log);
    log.entries[0].packet_id = 555U;
    log.entries[0].from = 0xAAAAU;
    log.entries[0].to = 0x1U;
    log.entries[1].packet_id = 555U;
    log.entries[1].from = 0xBBBBU;
    log.entries[1].to = 0x1U;
    log.count = 2U;
    (void)log_entries;

    if (mesh_message_log_forget_message(&log, 0xAAAAU, 0U, 555U) != 1U || log.count != 1U ||
        log.entries[0].from != 0xBBBBU) {
        failure = "the ring's delete should stay in its conversation too";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
