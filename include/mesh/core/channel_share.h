#ifndef MESH_CORE_CHANNEL_SHARE_H
#define MESH_CORE_CHANNEL_SHARE_H

/*
 * The two directions of channel sharing, between a radio's channel table and a `ChannelSet`.
 *
 * mesh/proto/channel_url.h turns a ChannelSet into a link and back; this is the half that knows
 * what a radio is. The two are separate for the reason the wire formats are separate from the
 * store everywhere else here: the link is the same eight messages whatever is holding them, and
 * *which slot a channel lives in, what role it has and which admin writes move it* is a fact
 * about this radio and this client's queue.
 *
 * Sharing out is the easy direction and the one that matters most: the Brick has no camera, so
 * it can never scan a code, but it can show one - and a person handing a channel to a group is
 * surrounded by phones that can read it. Importing is the other half, and reaches this client
 * by somebody typing the link in.
 */

#include "mesh/core/radio_settings.h"
#include "meshtastic/apponly.pb.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Builds the set this radio is on: its primary first, then every secondary, then its LoRa
 * config. Returns how many channels went in, 0 when the radio's table has not arrived or holds
 * no primary.
 *
 * Disabled slots are dropped rather than carried as holes, which is what `ChannelSet` means by
 * "the most compact possible representation" - and what a reader joining will expect, since a
 * hole in the middle of somebody else's table says nothing about theirs.
 */
size_t mesh_channel_share_build(const struct mesh_radio_settings *settings,
                                meshtastic_ChannelSet *out);

/* The same thing as a link, through mesh_channel_url_encode(). Returns the characters written,
   0 when there is nothing to share or the buffer is too small. */
size_t mesh_channel_share_url(const struct mesh_radio_settings *settings, char *out,
                              size_t out_len);

/*
 * What importing a set would do to this radio, without doing it.
 *
 * Every field here is what the caller needs to *ask the question* - a sheet that says "this
 * replaces your primary" when it would not is worse than no sheet - so it is worked out once,
 * by the code that will do the writing, rather than guessed at twice.
 */
struct mesh_channel_import_plan {
    size_t writes;          /* channel slots that would be written */
    bool replaces_primary;  /* the radio's primary channel would change */
    bool writes_lora;       /* the set carries a LoRa config and it differs from this radio's */
    bool full;              /* channels had to be dropped: the set is wider than the table */
    size_t channels;        /* channels in the set, dropped ones included */
};

/*
 * Works out what `set` would do to `settings`, in one of the two modes the URL distinguishes.
 *
 * `add` is the `?add=true` the apps send: keep this radio's primary and existing channels and
 * put the incoming ones in whatever slots are free, as secondaries. Without it the set replaces
 * the table - its first channel becomes the primary, the rest become secondaries in order, and
 * any slot past them that is in use is disabled.
 *
 * Returns false only on a NULL argument or a radio whose channel table has not arrived; a plan
 * with `writes` of 0 is a legitimate answer meaning this radio is already on that set.
 */
bool mesh_channel_import_plan(const struct mesh_radio_settings *settings,
                              const meshtastic_ChannelSet *set, bool add,
                              struct mesh_channel_import_plan *out);

/*
 * Queues the writes that plan describes, channels first and the LoRa config last.
 *
 * That order is not tidiness. A LoRa write is what makes the firmware reboot, and a radio that
 * rebooted half way through would come back on some of the channels - so the write that ends
 * the conversation goes last, and the channels are already in flash by the time it lands.
 *
 * Returns the number of requests queued, 0 when there was nothing to do, or a negative errno:
 * -EINVAL for a NULL argument or a table that has not arrived, -ENOSPC when the queue cannot
 * take the whole import. Nothing is queued in the -ENOSPC case: half an imported channel set is
 * a radio on a mesh that does not exist.
 */
int mesh_channel_share_queue_import(struct mesh_radio_settings *settings,
                                    const meshtastic_ChannelSet *set, bool add);

#ifdef __cplusplus
}
#endif

#endif /* MESH_CORE_CHANNEL_SHARE_H */
