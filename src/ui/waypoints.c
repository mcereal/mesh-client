#define _POSIX_C_SOURCE 200809L

/*
 * The Waypoints tab as data: the ordered list, and one place's rows.
 *
 * Nothing here draws anything, and nothing here holds state. The store publishes what the mesh
 * has shared; this file answers "which order", "how far away and which way", and "what does one
 * of them say about itself" - three questions the fb backend and the CLI backend both ask, and
 * the nav asks the first of on every clamp.
 */

#include "mesh/ui/waypoints.h"

#include "mesh/geo/coords.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/node_detail.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <stdio.h>
#include <string.h>

/* ---- distances and directions -------------------------------------------------------------- */

/* Where a distance stops being a walk and starts being a journey, in each system of units. */
#define MESH_UI_DISTANCE_KM_FROM 1000.0
#define MESH_UI_METRES_PER_FOOT 0.3048
#define MESH_UI_METRES_PER_MILE 1609.344

void mesh_ui_format_distance(double metres, bool imperial, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (!(metres >= 0.0)) {
        /* Also the NaN case, which is why the test is written this way round. */
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT));
        return;
    }

    if (imperial) {
        if (metres < MESH_UI_METRES_PER_MILE) {
            mesh_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_FT,
                            (unsigned)(metres / MESH_UI_METRES_PER_FOOT + 0.5));
            return;
        }
        mesh_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_MI, metres / MESH_UI_METRES_PER_MILE);
        return;
    }
    if (metres < MESH_UI_DISTANCE_KM_FROM) {
        mesh_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_M, (unsigned)(metres + 0.5));
        return;
    }
    mesh_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_KM, metres / MESH_UI_DISTANCE_KM_FROM);
}

/* The word for a compass point. The mapping is here rather than in geo/ for the reason the
   delivery marks' words are in the UI: geo answers with a direction, and what a direction is
   called is the reader's language. */
static enum mesh_str_id waypoint_compass_str(enum mesh_geo_compass point) {
    static const enum mesh_str_id k_points[MESH_GEO_COMPASS_COUNT] = {
        MESH_STR_COMPASS_N, MESH_STR_COMPASS_NE, MESH_STR_COMPASS_E, MESH_STR_COMPASS_SE,
        MESH_STR_COMPASS_S, MESH_STR_COMPASS_SW, MESH_STR_COMPASS_W, MESH_STR_COMPASS_NW,
    };
    if ((unsigned)point >= (unsigned)MESH_GEO_COMPASS_COUNT) {
        return MESH_STR_COMPASS_N;
    }
    return k_points[point];
}

bool mesh_ui_waypoint_format_range(int32_t from_latitude_i, int32_t from_longitude_i,
                                   int32_t to_latitude_i, int32_t to_longitude_i, bool imperial,
                                   char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';

    struct mesh_geo_vector vector;
    if (!mesh_geo_vector_between(from_latitude_i, from_longitude_i, to_latitude_i, to_longitude_i,
                                 &vector)) {
        return false;
    }

    char distance[MESH_UI_WAYPOINT_RANGE_MAX];
    mesh_ui_format_distance(vector.distance_m, imperial, distance, sizeof distance);
    if (!vector.has_bearing) {
        /* Standing on it. A compass point here would be a direction to walk in from a place we
           are already at, which is the one thing the geometry refuses to invent. */
        mesh_str_copy(out, out_len, distance);
        return true;
    }
    mesh_str_format(out, out_len, MESH_STR_WAYPOINT_VAL_RANGE, distance,
                    mesh_str(waypoint_compass_str(mesh_geo_compass_of(vector.bearing_deg))));
    return true;
}

/* ---- the roster, as this screen needs it ----------------------------------------------------- */

bool mesh_ui_waypoint_our_fix(const struct mesh_ui_handshake_state *handshake,
                              int32_t *out_latitude_i, int32_t *out_longitude_i) {
    if (handshake == NULL || !handshake->has_my_info || handshake->my_info.node_num == 0U) {
        return false;
    }
    const struct mesh_ui_node_summary *self =
        mesh_ui_node_detail_find(handshake, handshake->my_info.node_num);
    if (self == NULL || !self->position.valid) {
        return false;
    }
    /* Range-checked again on the way out rather than trusted because it is ours: the roster is
       restored from a cache written by an older build, and a coordinate is checked where it is
       used as one. */
    if (!mesh_geo_coords_valid(self->position.latitude_i, self->position.longitude_i)) {
        return false;
    }
    if (out_latitude_i != NULL) {
        *out_latitude_i = self->position.latitude_i;
    }
    if (out_longitude_i != NULL) {
        *out_longitude_i = self->position.longitude_i;
    }
    return true;
}

/* A node number as a name, on the same fallback ladder the rest of the UI uses: the long name,
   then the short one, then the "!hex" id every phone app falls back to. */
static void waypoint_node_name(const struct mesh_ui_handshake_state *handshake, uint32_t node_id,
                               char *out, size_t out_len) {
    const struct mesh_ui_node_summary *node =
        handshake != NULL ? mesh_ui_node_detail_find(handshake, node_id) : NULL;
    if (node != NULL && node->long_name[0] != '\0') {
        mesh_str_copy(out, out_len, node->long_name);
        return;
    }
    if (node != NULL && node->short_name[0] != '\0') {
        mesh_str_copy(out, out_len, node->short_name);
        return;
    }
    mesh_str_format(out, out_len, MESH_STR_NODE_VAL_USER_ID_HEX, node_id);
}

/* Who shared a place, in the terms a row wants: our own waypoints say "you" rather than naming
   the radio, because the reader is the radio. */
static void waypoint_sharer_name(const struct mesh_ui_waypoint *waypoint,
                                 const struct mesh_ui_handshake_state *handshake, char *out,
                                 size_t out_len) {
    if (waypoint->ours) {
        mesh_str_copy(out, out_len, mesh_str(MESH_STR_WAYPOINTS_BY_YOU));
        return;
    }
    if (waypoint->from_name[0] != '\0') {
        mesh_str_copy(out, out_len, waypoint->from_name);
        return;
    }
    if (waypoint->from == 0U) {
        mesh_str_copy(out, out_len, mesh_str(MESH_STR_COMMON_UNKNOWN));
        return;
    }
    waypoint_node_name(handshake, waypoint->from, out, out_len);
}

/* The name a row shows: the sharer's, or the stand-in for one who left it empty. */
static void waypoint_display_name(const struct mesh_ui_waypoint *waypoint, char *out,
                                  size_t out_len) {
    mesh_str_copy(out, out_len,
                  waypoint->name[0] != '\0' ? waypoint->name
                                            : mesh_str(MESH_STR_WAYPOINTS_UNNAMED));
}

/* "4m ago", "3h ago" - the node detail's shorthand, so the two screens agree about ages. */
static void waypoint_format_age(uint32_t stamp, uint32_t now, char *out, size_t out_len) {
    if (stamp == 0U || now == 0U || stamp > now) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT));
        return;
    }
    const uint32_t seconds = now - stamp;
    if (seconds < 60U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_AGO_SECONDS, seconds);
    } else if (seconds < 3600U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_AGO_MINUTES, seconds / 60U);
    } else if (seconds < 86400U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_AGO_HOURS, seconds / 3600U);
    } else {
        mesh_str_format(out, out_len, MESH_STR_TIME_AGO_DAYS, seconds / 86400U);
    }
}

/* ---- the list -------------------------------------------------------------------------------- */

const struct mesh_ui_waypoint *mesh_ui_waypoint_find(const struct mesh_ui_waypoint_list *list,
                                                     uint32_t id) {
    if (list == NULL || id == 0U) {
        return NULL;
    }
    for (uint32_t i = 0; i < list->count && i < MESH_UI_MAX_WAYPOINTS; ++i) {
        if (list->entries[i].id == id) {
            return &list->entries[i];
        }
    }
    return NULL;
}

/*
 * One place's sort key, and the whole of the list's ordering rule.
 *
 * `group` first: a place we can measure comes before one we cannot, whatever either of them is.
 * Within the measurable group the key is the distance in metres, nearest first, because that is
 * the answer the tab exists to give. Within the other it is how recently we heard it, newest
 * first - the only thing left that distinguishes one unplaceable name from another.
 */
struct waypoint_key {
    uint8_t index;
    uint8_t group;
    double distance_m;
    uint32_t heard;
};

static uint32_t waypoint_places(const struct mesh_ui_store *store) {
    if (store == NULL) {
        return 0U;
    }
    return store->waypoints.count > MESH_UI_MAX_WAYPOINTS ? MESH_UI_MAX_WAYPOINTS
                                                          : store->waypoints.count;
}

/* Fills `order` with the entry indices, nearest first. Returns how many were written. */
static uint32_t waypoint_order(const struct mesh_ui_store *store, uint8_t *order) {
    const uint32_t count = waypoint_places(store);
    if (count == 0U) {
        return 0U;
    }

    int32_t self_lat = 0;
    int32_t self_lon = 0;
    const bool have_fix = mesh_ui_waypoint_our_fix(
        store->handshake_valid ? &store->handshake : NULL, &self_lat, &self_lon);

    struct waypoint_key keys[MESH_UI_MAX_WAYPOINTS];
    for (uint32_t i = 0; i < count; ++i) {
        const struct mesh_ui_waypoint *waypoint = &store->waypoints.entries[i];
        keys[i].index = (uint8_t)i;
        keys[i].group = 1U;
        keys[i].distance_m = 0.0;
        keys[i].heard = waypoint->heard;
        if (have_fix && waypoint->has_coords) {
            struct mesh_geo_vector vector;
            if (mesh_geo_vector_between(self_lat, self_lon, waypoint->latitude_i,
                                        waypoint->longitude_i, &vector)) {
                keys[i].group = 0U;
                keys[i].distance_m = vector.distance_m;
            }
        }
    }

    /* Insertion sort: the list is 32 entries at most and is rebuilt per frame, so the simplest
       stable sort is the right one - and stability is what keeps two places at the same
       distance from swapping under the cursor between frames. */
    for (uint32_t i = 1; i < count; ++i) {
        const struct waypoint_key key = keys[i];
        uint32_t j = i;
        while (j > 0U) {
            const struct waypoint_key *prev = &keys[j - 1U];
            bool after = false;
            if (key.group != prev->group) {
                after = key.group < prev->group;
            } else if (key.group == 0U) {
                after = key.distance_m < prev->distance_m;
            } else {
                after = key.heard > prev->heard;
            }
            if (!after) {
                break;
            }
            keys[j] = keys[j - 1U];
            --j;
        }
        keys[j] = key;
    }

    for (uint32_t i = 0; i < count; ++i) {
        order[i] = keys[i].index;
    }
    return count;
}

uint32_t mesh_ui_waypoint_count(const struct mesh_ui_store *store) {
    /* Always at least the "New waypoint here" row: a screen with no rows at all cannot be
       navigated, and this one always has something to offer. */
    return waypoint_places(store) + 1U;
}

bool mesh_ui_waypoint_row(const struct mesh_ui_store *store, uint32_t index,
                          struct mesh_ui_waypoint_row *out) {
    if (store == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);

    const uint32_t places = waypoint_places(store);
    if (index > places) {
        return false;
    }
    if (index == places) {
        out->type = MESH_UI_WAYPOINT_ROW_NEW;
        mesh_str_copy(out->name, sizeof out->name, mesh_str(MESH_STR_WAYPOINTS_NEW));
        /*
         * The row says why it cannot be pressed, on the supporting line rather than in the
         * range column. A range is a measurement between two points and the trailing column is
         * where the reader looks for one; a reason is a sentence, and putting it there was the
         * delivery-mark mistake one screen over - fifteen right-aligned characters standing in
         * for an explanation, on a row whose supporting line was meanwhile talking about
         * something else. The press says the same thing out loud, because the row saying it is
         * no use to somebody who has already pressed.
         */
        int32_t lat = 0;
        int32_t lon = 0;
        if (!mesh_ui_waypoint_our_fix(store->handshake_valid ? &store->handshake : NULL, &lat,
                                      &lon)) {
            mesh_str_copy(out->shared, sizeof out->shared,
                          mesh_str(MESH_STR_WAYPOINTS_NEW_NEEDS_FIX));
        }
        return true;
    }

    uint8_t order[MESH_UI_MAX_WAYPOINTS];
    const uint32_t ordered = waypoint_order(store, order);
    if (index >= ordered) {
        return false;
    }
    const struct mesh_ui_waypoint *waypoint = &store->waypoints.entries[order[index]];
    const struct mesh_ui_handshake_state *handshake =
        store->handshake_valid ? &store->handshake : NULL;

    out->type = MESH_UI_WAYPOINT_ROW_PLACE;
    out->id = waypoint->id;
    out->waypoint = waypoint;
    out->editable = waypoint->editable;
    out->ours = waypoint->ours;
    waypoint_display_name(waypoint, out->name, sizeof out->name);

    int32_t self_lat = 0;
    int32_t self_lon = 0;
    if (waypoint->has_coords && mesh_ui_waypoint_our_fix(handshake, &self_lat, &self_lon)) {
        (void)mesh_ui_waypoint_format_range(self_lat, self_lon, waypoint->latitude_i,
                                            waypoint->longitude_i, store->settings.units == 1U,
                                            out->range, sizeof out->range);
    }

    char sharer[MESH_UI_NAV_TARGET_NAME_MAX];
    waypoint_sharer_name(waypoint, handshake, sharer, sizeof sharer);
    char age[24];
    /* The wall clock, asked for here rather than passed in - the same call fb_render_node_detail
       makes for the same reason, and the same one mesh_time_wall_set_fixed() pins in a test. The
       credible one, because a Brick with no network boots into 1970 and every age measured
       against that comes out as decades; 0 yields "?", which is the honest answer. */
    waypoint_format_age(waypoint->heard, mesh_time_wall_credible_s(), age, sizeof age);
    mesh_str_format(out->shared, sizeof out->shared, MESH_STR_WAYPOINTS_ROW_SHARED, sharer, age);
    return true;
}

/* ---- one waypoint's detail --------------------------------------------------------------------
 */

struct waypoint_rows {
    struct mesh_ui_waypoint_item *items;
    uint32_t count;
    uint32_t capacity;
};

/* Counts past the end rather than clamping, exactly as the node detail's builder does: a screen
   that quietly stopped emitting rows would be a screen whose budget nothing could check. */
static struct mesh_ui_waypoint_item *rows_next(struct waypoint_rows *rows) {
    struct mesh_ui_waypoint_item *item =
        (rows->count < rows->capacity) ? &rows->items[rows->count] : NULL;
    rows->count++;
    if (item != NULL) {
        memset(item, 0, sizeof *item);
    }
    return item;
}

static void rows_heading(struct waypoint_rows *rows, enum mesh_str_id label) {
    struct mesh_ui_waypoint_item *item = rows_next(rows);
    if (item == NULL) {
        return;
    }
    item->kind = MESH_UI_WAYPOINT_ITEM_HEADING;
    mesh_str_copy(item->label, sizeof item->label, mesh_str(label));
}

static void rows_text(struct waypoint_rows *rows, enum mesh_str_id label, const char *value) {
    struct mesh_ui_waypoint_item *item = rows_next(rows);
    if (item == NULL) {
        return;
    }
    item->kind = MESH_UI_WAYPOINT_ITEM_INFO;
    mesh_str_copy(item->label, sizeof item->label, mesh_str(label));
    mesh_str_copy(item->value, sizeof item->value, value);
}

static void rows_note(struct waypoint_rows *rows, const char *text) {
    struct mesh_ui_waypoint_item *item = rows_next(rows);
    if (item == NULL) {
        return;
    }
    item->kind = MESH_UI_WAYPOINT_ITEM_NOTE;
    mesh_str_copy(item->value, sizeof item->value, text);
}

static void rows_action(struct waypoint_rows *rows, enum mesh_str_id label, const char *value,
                        enum mesh_ui_waypoint_action action) {
    struct mesh_ui_waypoint_item *item = rows_next(rows);
    if (item == NULL) {
        return;
    }
    item->kind = MESH_UI_WAYPOINT_ITEM_ACTION;
    item->action = (uint8_t)action;
    mesh_str_copy(item->label, sizeof item->label, mesh_str(label));
    mesh_str_copy(item->value, sizeof item->value, value != NULL ? value : "");
}

static void waypoint_rows_place(struct waypoint_rows *rows, const struct mesh_ui_waypoint *waypoint,
                                const struct mesh_ui_handshake_state *handshake, bool imperial) {
    rows_heading(rows, MESH_STR_WAYPOINT_HEAD_PLACE);

    /*
     * The range, and when there is none, which of the two reasons it is.
     *
     * They are different facts about different things - one about this waypoint, one about our
     * own radio - and only one of them is something the reader can act on: a radio with no fix
     * has a fixed position waiting for it in Settings.
     */
    int32_t self_lat = 0;
    int32_t self_lon = 0;
    char range[MESH_UI_WAYPOINT_RANGE_MAX];
    if (!waypoint->has_coords) {
        rows_text(rows, MESH_STR_WAYPOINT_RANGE, mesh_str(MESH_STR_WAYPOINT_RANGE_NO_COORDS));
    } else if (!mesh_ui_waypoint_our_fix(handshake, &self_lat, &self_lon)) {
        rows_text(rows, MESH_STR_WAYPOINT_RANGE, mesh_str(MESH_STR_WAYPOINT_RANGE_NO_FIX));
    } else if (mesh_ui_waypoint_format_range(self_lat, self_lon, waypoint->latitude_i,
                                             waypoint->longitude_i, imperial, range,
                                             sizeof range)) {
        rows_text(rows, MESH_STR_WAYPOINT_RANGE, range);
    }

    if (waypoint->has_coords) {
        char value[MESH_UI_WAYPOINT_RANGE_MAX];
        mesh_str_format(value, sizeof value, MESH_STR_NODE_VAL_DEGREES,
                        (double)waypoint->latitude_i / 1e7);
        rows_text(rows, MESH_STR_WAYPOINT_LATITUDE, value);
        mesh_str_format(value, sizeof value, MESH_STR_NODE_VAL_DEGREES,
                        (double)waypoint->longitude_i / 1e7);
        rows_text(rows, MESH_STR_WAYPOINT_LONGITUDE, value);
    }
}

/* The channel a place was shared on, by name when the radio has told us one. */
static void waypoint_channel_name(const struct mesh_ui_handshake_state *handshake, uint8_t channel,
                                  char *out, size_t out_len) {
    if (handshake != NULL) {
        for (uint32_t i = 0; i < handshake->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
            if (handshake->channels[i].index == channel && handshake->channels[i].name[0] != '\0') {
                mesh_str_copy(out, out_len, handshake->channels[i].name);
                return;
            }
        }
    }
    mesh_str_format(out, out_len, MESH_STR_CHANNEL_NUMBERED, (unsigned)channel);
}

static void waypoint_rows_shared(struct waypoint_rows *rows,
                                 const struct mesh_ui_waypoint *waypoint,
                                 const struct mesh_ui_handshake_state *handshake, uint32_t now) {
    rows_heading(rows, MESH_STR_WAYPOINT_HEAD_SHARED);

    char value[MESH_UI_WAYPOINT_VALUE_MAX];
    waypoint_sharer_name(waypoint, handshake, value, sizeof value);
    rows_text(rows, MESH_STR_WAYPOINT_SHARED_BY, value);

    waypoint_channel_name(handshake, waypoint->channel, value, sizeof value);
    rows_text(rows, MESH_STR_WAYPOINT_CHANNEL, value);

    waypoint_format_age(waypoint->heard, now, value, sizeof value);
    rows_text(rows, MESH_STR_WAYPOINT_HEARD, value);

    if (waypoint->expire == 0U) {
        rows_text(rows, MESH_STR_WAYPOINT_EXPIRES, mesh_str(MESH_STR_WAYPOINT_EXPIRES_NEVER));
    } else if (now != 0U && waypoint->expire > now) {
        const uint32_t left = waypoint->expire - now;
        char remaining[24];
        if (left < 3600U) {
            mesh_str_format(remaining, sizeof remaining, MESH_STR_TIME_MINUTES_SHORT, left / 60U);
        } else if (left < 86400U) {
            mesh_str_format(remaining, sizeof remaining, MESH_STR_TIME_HOURS_SHORT, left / 3600U);
        } else {
            mesh_str_format(remaining, sizeof remaining, MESH_STR_TIME_DAYS_SHORT, left / 86400U);
        }
        mesh_str_format(value, sizeof value, MESH_STR_WAYPOINT_EXPIRES_IN, remaining);
        rows_text(rows, MESH_STR_WAYPOINT_EXPIRES, value);
    }
    /* An expiry we cannot place against a clock draws no row at all. Saying "?" would be the
       screen asserting there is a deadline it cannot read, when the honest position is that a
       Brick has no wall clock and the mesh's own copy is the authority. */

    if (waypoint->locked_to == 0U) {
        rows_text(rows, MESH_STR_WAYPOINT_LOCKED_TO, mesh_str(MESH_STR_WAYPOINT_LOCKED_OPEN));
    } else {
        waypoint_node_name(handshake, waypoint->locked_to, value, sizeof value);
        rows_text(rows, MESH_STR_WAYPOINT_LOCKED_TO, value);
    }
}

uint32_t mesh_ui_waypoint_detail_build(const struct mesh_ui_waypoint *waypoint,
                                       const struct mesh_ui_handshake_state *handshake,
                                       const struct mesh_ui_settings *settings, uint32_t now,
                                       bool delete_armed, struct mesh_ui_waypoint_item *out,
                                       uint32_t capacity) {
    if (waypoint == NULL || out == NULL || capacity == 0U) {
        return 0U;
    }
    struct waypoint_rows rows = {.items = out, .count = 0U, .capacity = capacity};

    waypoint_rows_place(&rows, waypoint, handshake, settings != NULL && settings->units == 1U);

    if (waypoint->description[0] != '\0') {
        rows_heading(&rows, MESH_STR_WAYPOINT_HEAD_NOTE);
        rows_note(&rows, waypoint->description);
    }

    waypoint_rows_shared(&rows, waypoint, handshake, now);

    rows_heading(&rows, MESH_STR_WAYPOINT_HEAD_ACTIONS);
    rows_action(&rows, MESH_STR_WAYPOINT_ACT_SHARE, mesh_str(MESH_STR_COMMON_PRESS_A),
                MESH_UI_WAYPOINT_ACTION_SHARE);
    /*
     * The delete row says which of the two deletes it is before it is pressed.
     *
     * A place locked to somebody else cannot be withdrawn from the mesh by us - the firmware
     * would ignore it and every other client would keep showing it - so the row offers the only
     * thing that is true: this client will stop showing it. Labelling both "Delete" would make
     * the honest case look like the destructive one.
     */
    rows_action(
        &rows, waypoint->editable ? MESH_STR_WAYPOINT_ACT_DELETE : MESH_STR_WAYPOINT_ACT_FORGET,
        mesh_str(delete_armed ? MESH_STR_WAYPOINT_ACT_DELETE_ARMED : MESH_STR_COMMON_PRESS_A),
        MESH_UI_WAYPOINT_ACTION_DELETE);

    return rows.count < capacity ? rows.count : capacity;
}
