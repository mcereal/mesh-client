#include "mesh/core/tern.h"

#include "inkwell/base/array.h"
#include "inkwell/base/text.h"
#include "inkwell/codec/sha256.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/*
 * Every frame is its type's fields in a fixed order, so the codec is one table: a row per type,
 * naming the fields the draft lists for it, each as a kind and the member of struct
 * mesh_tern_frame it lands in. Reading and writing walk the same row, which is what keeps the
 * two from drifting apart. SET is the one frame whose last field depends on an earlier one, and
 * the walk picks its value's kind from `setting`, and a setting carries a version too.
 *
 * A row and a field each carry the version that first defines it. A later version only adds types
 * and fields at the end of a frame, so a reader of an earlier one stops at the first field it does
 * not define, and takes a type it does not define for unknown before looking at its bytes.
 */

enum mesh_tern_kind {
    TERN_END = 0,
    TERN_U8,
    TERN_I8,
    TERN_U16,
    TERN_I16,
    TERN_U32,
    TERN_I32,
    TERN_ADDR,
    TERN_GID,
    TERN_DIGEST,
    TERN_STR,       /* `max` is the longest it may be */
    TERN_BYTES,     /* a `str` that need not be UTF-8 */
    TERN_SET_VALUE, /* SET's value, as `setting` says */
};

/*
 * One field: its kind on the wire, and where it lands - `offset` and `width` of the member for a
 * number (which may be wider than the field, see struct mesh_tern_frame), `offset` of the bytes
 * and `len_offset` of their length for a string. `since` is the version that added the field.
 */
struct mesh_tern_field {
    uint8_t kind;
    uint8_t max;
    uint8_t width;
    uint8_t since;
    uint16_t offset;
    uint16_t len_offset;
};

#define MEMBER(member) offsetof(struct mesh_tern_frame, member)
#define WIDTH(member) sizeof(((struct mesh_tern_frame *)0)->member)
#define F_NUM_SINCE(kind, member, since)                                                           \
    { (kind), 0U, (uint8_t)WIDTH(member), (since), (uint16_t)MEMBER(member), 0U }
#define F_U8(member) F_NUM_SINCE(TERN_U8, member, 0U)
#define F_I8(member) F_NUM_SINCE(TERN_I8, member, 0U)
#define F_U16(member) F_NUM_SINCE(TERN_U16, member, 0U)
#define F_I16(member) F_NUM_SINCE(TERN_I16, member, 0U)
#define F_U32(member) F_NUM_SINCE(TERN_U32, member, 0U)
#define F_I32(member) F_NUM_SINCE(TERN_I32, member, 0U)
#define F_FIXED(kind, member)                                                                      \
    { (kind), 0U, 0U, 0U, (uint16_t)MEMBER(member), 0U }
#define F_ADDR F_FIXED(TERN_ADDR, address)
#define F_GID F_FIXED(TERN_GID, group)
#define F_DIGEST F_FIXED(TERN_DIGEST, digest)
#define F_STRING_SINCE(kind, member, len, max, since)                                              \
    { (kind), (uint8_t)(max), 0U, (since), (uint16_t)MEMBER(member), (uint16_t)MEMBER(len) }
#define F_STR(max) F_STRING_SINCE(TERN_STR, text, text_len, max, 0U)
#define F_SET                                                                                      \
    { TERN_SET_VALUE, 0U, 0U, 0U, 0U, 0U }

/* A position's fields after its sender: the precision, the cell's centre, altitude, accuracy (a
   byte as news) and age (four bytes as news). */
#define F_POSITION                                                                                 \
    F_U8(precision), F_I32(lat), F_I32(lon), F_I16(altitude), F_U8(accuracy), F_U32(age)
/* How the node shares its position, after whom with. */
#define F_SHARING F_U8(precision), F_U8(fields), F_U16(interval), F_U16(minutes)
#define F_ITEM_STATE F_U8(flags), F_U8(state), F_U8(reason), F_U16(wait)

/* The most fields a type has: GROUP_MESSAGE's and INVITE's nine, and the terminator. */
#define TERN_FIELDS_MAX 10U

static const struct {
    uint8_t type;
    uint8_t since;
    struct mesh_tern_field fields[TERN_FIELDS_MAX];
} k_frames[] = {
    {MESH_TERN_HELLO, 0U, {F_U8(version)}},
    {MESH_TERN_SYNC, 0U, {F_U32(after)}},
    {MESH_TERN_PING, 0U, {{0}}},
    {MESH_TERN_SET_TIME, 0U, {F_U32(time)}},
    {MESH_TERN_SET, 0U, {F_U8(setting), F_SET}},
    {MESH_TERN_OFF, 9U, {{0}}},
    {MESH_TERN_SEND, 0U, {F_U32(ref), F_ADDR, F_STR(MESH_TERN_TEXT_MAX)}},
    {MESH_TERN_READ, 0U, {F_U32(through)}},
    {MESH_TERN_SAVE_CONTACT, 0U, {F_ADDR, F_STR(MESH_TERN_NAME_MAX)}},
    {MESH_TERN_REMOVE_CONTACT, 0U, {F_ADDR}},
    {MESH_TERN_END_SESSION, 1U, {F_ADDR}},
    {MESH_TERN_MAKE_GROUP, 2U, {F_STR(MESH_TERN_NAME_MAX)}},
    {MESH_TERN_LEAVE_GROUP, 2U, {F_GID}},
    {MESH_TERN_NAME_GROUP, 2U, {F_GID, F_STR(MESH_TERN_NAME_MAX)}},
    {MESH_TERN_SEND_GROUP, 2U, {F_U32(ref), F_GID, F_STR(MESH_TERN_TEXT_MAX)}},
    {MESH_TERN_SEND_INVITE, 2U, {F_GID, F_ADDR}},
    {MESH_TERN_JOIN, 2U, {F_U32(id)}},
    {MESH_TERN_GROUP_LINK, 7U, {F_GID}},
    {MESH_TERN_JOIN_LINK, 7U, {F_STR(MESH_TERN_LINK_MAX)}},
    {MESH_TERN_UPDATE_BEGIN, 4U, {F_U32(size), F_DIGEST}},
    {MESH_TERN_UPDATE_DATA,
     4U,
     {F_U32(offset), F_STRING_SINCE(TERN_BYTES, data, data_len, MESH_TERN_UPDATE_CHUNK, 0U)}},
    {MESH_TERN_UPDATE_END, 4U, {{0}}},
    {MESH_TERN_SET_POSITION,
     5U,
     {F_I32(lat), F_I32(lon), F_I16(altitude), F_U16(accuracy), F_U16(age)}},
    {MESH_TERN_SHARE, 5U, {F_ADDR, F_SHARING}},
    {MESH_TERN_SHARE_GROUP, 5U, {F_GID, F_SHARING}},
    {MESH_TERN_OK, 0U, {{0}}},
    {MESH_TERN_ERROR, 0U, {F_U8(code)}},
    {MESH_TERN_INFO,
     0U,
     {F_U8(version), F_STR(MESH_TERN_FIRMWARE_MAX),
      F_STRING_SINCE(TERN_STR, board, board_len, MESH_TERN_FIRMWARE_MAX, 4U),
      F_STRING_SINCE(TERN_STR, release, release_len, MESH_TERN_FIRMWARE_MAX, 4U)}},
    {MESH_TERN_SYNCED, 0U, {F_NUM_SINCE(TERN_U8, news, 3U)}},
    {MESH_TERN_QUEUED, 0U, {F_U32(id)}},
    {MESH_TERN_MADE, 2U, {F_GID}},
    {MESH_TERN_UPDATING, 4U, {F_U32(offset)}},
    {MESH_TERN_LINK, 7U, {F_STR(MESH_TERN_LINK_MAX)}},
    {MESH_TERN_SELF,
     0U,
     {F_ADDR, F_U8(role), F_STR(MESH_TERN_REGION_MAX), F_I8(power), F_U32(time),
      F_NUM_SINCE(TERN_U8, cards, 6U),
      F_STRING_SINCE(TERN_STR, card_name, card_name_len, MESH_TERN_NAME_MAX, 6U)}},
    {MESH_TERN_CONTACT, 0U, {F_ADDR, F_U8(session), F_STR(MESH_TERN_NAME_MAX)}},
    {MESH_TERN_CONTACT_GONE, 0U, {F_ADDR}},
    {MESH_TERN_MESSAGE,
     0U,
     {F_U32(id), F_ADDR, F_U32(time), F_ITEM_STATE, F_STR(MESH_TERN_TEXT_MAX)}},
    {MESH_TERN_STATE, 0U, {F_U32(id), F_U8(state), F_U8(reason), F_U16(wait)}},
    {MESH_TERN_NEIGHBOUR, 0U, {F_U32(routing_id), F_U8(role), F_I8(snr), F_U16(heard)}},
    {MESH_TERN_NEIGHBOUR_GONE, 0U, {F_U32(routing_id)}},
    {MESH_TERN_AIRTIME, 0U, {F_U32(period), F_U32(allowed), F_U32(used), F_U32(wait)}},
    {MESH_TERN_POWER, 0U, {F_U16(millivolts), F_U8(percent), F_U8(flags)}},
    {MESH_TERN_ASKED, 1U, {F_ADDR, F_U8(why)}},
    {MESH_TERN_GROUP, 2U, {F_GID, F_STR(MESH_TERN_NAME_MAX)}},
    {MESH_TERN_GROUP_GONE, 2U, {F_GID}},
    {MESH_TERN_GROUP_MESSAGE,
     2U,
     {F_U32(id), F_GID, F_U32(from), F_U32(time), F_ITEM_STATE, F_STR(MESH_TERN_TEXT_MAX)}},
    {MESH_TERN_INVITE,
     2U,
     {F_U32(id), F_ADDR, F_GID, F_U32(time), F_ITEM_STATE, F_STR(MESH_TERN_NAME_MAX)}},
    {MESH_TERN_POSITION, 5U, {F_ADDR, F_POSITION}},
    {MESH_TERN_GROUP_POSITION, 5U, {F_GID, F_U32(from), F_POSITION}},
    {MESH_TERN_SHARING, 5U, {F_ADDR, F_SHARING}},
    {MESH_TERN_GROUP_SHARING, 5U, {F_GID, F_SHARING}},
    {MESH_TERN_CARD, 6U, {F_ADDR, F_U32(heard), F_STR(MESH_TERN_NAME_MAX)}},
    {MESH_TERN_CARD_GONE, 6U, {F_ADDR}},
    /* The node's own fix: accuracy two bytes, as SET_POSITION's, and age four, as POSITION's. */
    {MESH_TERN_FIX,
     8U,
     {F_U8(receiver), F_U8(source), F_I32(lat), F_I32(lon), F_I16(altitude), F_U16(accuracy),
      F_U32(age), F_U8(satellites)}},
};

/* The fields of `type` as `version` defines them, or NULL for a type it does not. */
static const struct mesh_tern_field *mesh_tern_fields(uint8_t type, uint8_t version) {
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(k_frames); ++i) {
        if (k_frames[i].type == type) {
            return k_frames[i].since <= version ? k_frames[i].fields : NULL;
        }
    }
    return NULL;
}

uint8_t mesh_tern_since(uint8_t type) {
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(k_frames); ++i) {
        if (k_frames[i].type == type) {
            return k_frames[i].since;
        }
    }
    return UINT8_MAX;
}

uint8_t mesh_tern_setting_since(uint8_t setting) {
    switch ((enum mesh_tern_setting)setting) {
    case MESH_TERN_SETTING_REGION:
    case MESH_TERN_SETTING_ROLE:
    case MESH_TERN_SETTING_POWER:
    case MESH_TERN_SETTING_PASSKEY:
        return 0U;
    case MESH_TERN_SETTING_CARDS:
    case MESH_TERN_SETTING_CARD_NAME:
        return 6U;
    case MESH_TERN_SETTING_RECEIVER:
        return 8U;
    }
    return UINT8_MAX;
}

uint8_t mesh_tern_frame_since(const uint8_t *frame, size_t len) {
    if (frame == NULL || len < 1U) {
        return UINT8_MAX;
    }
    uint8_t since = mesh_tern_since(frame[0]);
    if (frame[0] == MESH_TERN_SET && len >= 3U) {
        const uint8_t setting = mesh_tern_setting_since(frame[2]);
        since = setting > since ? setting : since;
    }
    return since;
}

/* The kind and member SET's value takes, or false for a setting `version` lacks. */
static bool mesh_tern_set_field(uint8_t setting, uint8_t version, struct mesh_tern_field *out) {
    if (mesh_tern_setting_since(setting) > version) {
        return false;
    }
    switch ((enum mesh_tern_setting)setting) {
    case MESH_TERN_SETTING_REGION:
        *out = (struct mesh_tern_field)F_STR(MESH_TERN_REGION_MAX);
        return true;
    case MESH_TERN_SETTING_ROLE:
        *out = (struct mesh_tern_field)F_U8(role);
        return true;
    case MESH_TERN_SETTING_POWER:
        *out = (struct mesh_tern_field)F_I8(power);
        return true;
    case MESH_TERN_SETTING_PASSKEY:
        *out = (struct mesh_tern_field)F_U32(passkey);
        return true;
    case MESH_TERN_SETTING_CARDS:
        *out = (struct mesh_tern_field)F_U8(cards);
        return true;
    case MESH_TERN_SETTING_CARD_NAME:
        *out = (struct mesh_tern_field)F_STR(MESH_TERN_NAME_MAX);
        return true;
    case MESH_TERN_SETTING_RECEIVER:
        *out = (struct mesh_tern_field)F_U8(receiver);
        return true;
    }
    return false;
}

/* The version an INFO is read and written by: the lesser of the one spoken and its own. */
static uint8_t mesh_tern_info_version(uint8_t version, uint8_t carried) {
    return carried < version ? carried : version;
}

bool mesh_tern_utf8_valid(const uint8_t *text, size_t len) {
    size_t at = 0U;
    while (at < len) {
        const size_t step = inkwell_text_utf8_sequence_len(text + at, len - at);
        if (step == 0U) {
            return false;
        }
        at += step;
    }
    return true;
}

static uint32_t mesh_tern_be(const uint8_t *p, size_t bytes) {
    uint32_t value = 0U;
    for (size_t i = 0; i < bytes; ++i) {
        value = (value << 8U) | p[i];
    }
    return value;
}

static void mesh_tern_put_be(uint8_t *p, uint32_t value, size_t bytes) {
    for (size_t i = bytes; i > 0U; --i) {
        p[i - 1U] = (uint8_t)(value & 0xFFU);
        value >>= 8U;
    }
}

static size_t mesh_tern_kind_size(uint8_t kind) {
    switch (kind) {
    case TERN_U8:
    case TERN_I8:
        return 1U;
    case TERN_U16:
    case TERN_I16:
        return 2U;
    case TERN_U32:
    case TERN_I32:
        return 4U;
    case TERN_ADDR:
    case TERN_DIGEST:
        return 32U;
    case TERN_GID:
        return MESH_TERN_GROUP_LEN;
    default:
        return 0U;
    }
}

static bool mesh_tern_kind_signed(uint8_t kind) {
    return kind == TERN_I8 || kind == TERN_I16 || kind == TERN_I32;
}

/* One numeric field into its member, which may be wider: a signed field is sign-extended. */
static void mesh_tern_store_number(struct mesh_tern_frame *out, const struct mesh_tern_field *f,
                                   uint32_t value) {
    uint8_t *member = (uint8_t *)out + f->offset;
    const size_t size = mesh_tern_kind_size(f->kind);
    if (mesh_tern_kind_signed(f->kind) && size < 4U &&
        (value & ((uint32_t)1U << (size * 8U - 1U))) != 0U) {
        value |= ~(((uint32_t)1U << (size * 8U)) - 1U);
    }
    switch (f->width) {
    case 1U:
        *member = (uint8_t)value;
        break;
    case 2U: {
        const uint16_t v = (uint16_t)value;
        memcpy(member, &v, sizeof v);
        break;
    }
    case 4U:
        memcpy(member, &value, sizeof value);
        break;
    default:
        break;
    }
}

/* One numeric member, as the field's bytes carry it: an unsigned member wider than its field is
   written clamped to the field's largest value rather than cut. */
static uint32_t mesh_tern_load_number(const struct mesh_tern_frame *frame,
                                      const struct mesh_tern_field *f) {
    const uint8_t *member = (const uint8_t *)frame + f->offset;
    uint32_t value = 0U;
    switch (f->width) {
    case 1U:
        value = *member;
        break;
    case 2U: {
        uint16_t v = 0U;
        memcpy(&v, member, sizeof v);
        value = v;
        break;
    }
    case 4U:
        memcpy(&value, member, sizeof value);
        break;
    default:
        break;
    }
    const size_t size = mesh_tern_kind_size(f->kind);
    if (!mesh_tern_kind_signed(f->kind) && size < f->width) {
        const uint32_t most = ((uint32_t)1U << (size * 8U)) - 1U;
        value = value > most ? most : value;
    }
    return value;
}

enum mesh_tern_decode_result mesh_tern_decode(const uint8_t *frame, size_t len, uint8_t version,
                                              struct mesh_tern_frame *out) {
    if (frame == NULL || out == NULL || len < 2U) {
        return MESH_TERN_DECODE_SHORT;
    }
    memset(out, 0, sizeof *out);
    out->type = frame[0];
    out->seq = frame[1];
    const struct mesh_tern_field *fields = mesh_tern_fields(frame[0], version);
    if (fields == NULL) {
        return MESH_TERN_DECODE_UNKNOWN;
    }
    if (frame[0] == MESH_TERN_INFO && len >= 3U) {
        version = mesh_tern_info_version(version, frame[2]);
    }
    size_t at = 2U;
    for (size_t i = 0; i < TERN_FIELDS_MAX && fields[i].kind != TERN_END; ++i) {
        struct mesh_tern_field f = fields[i];
        if (f.since > version) {
            break; /* a later version's, and so is everything after it */
        }
        if (f.kind == TERN_SET_VALUE && !mesh_tern_set_field(out->setting, version, &f)) {
            return MESH_TERN_DECODE_UNKNOWN;
        }
        if (f.kind == TERN_STR || f.kind == TERN_BYTES) {
            if (at >= len) {
                return MESH_TERN_DECODE_MALFORMED;
            }
            const size_t n = frame[at];
            if (n > f.max || len - at - 1U < n) {
                return MESH_TERN_DECODE_MALFORMED;
            }
            if (f.kind == TERN_STR && !mesh_tern_utf8_valid(frame + at + 1U, n)) {
                return MESH_TERN_DECODE_MALFORMED;
            }
            uint8_t *bytes = (uint8_t *)out + f.offset;
            memcpy(bytes, frame + at + 1U, n);
            if (f.kind == TERN_STR) {
                bytes[n] = '\0';
            }
            *((uint8_t *)out + f.len_offset) = (uint8_t)n;
            at += 1U + n;
            continue;
        }
        const size_t size = mesh_tern_kind_size(f.kind);
        if (len - at < size) {
            return MESH_TERN_DECODE_MALFORMED;
        }
        if (f.kind == TERN_ADDR || f.kind == TERN_GID || f.kind == TERN_DIGEST) {
            memcpy((uint8_t *)out + f.offset, frame + at, size);
        } else {
            mesh_tern_store_number(out, &f, mesh_tern_be(frame + at, size));
        }
        at += size;
    }
    return MESH_TERN_DECODE_OK;
}

int mesh_tern_encode(const struct mesh_tern_frame *frame, uint8_t version, uint8_t *out,
                     size_t out_len) {
    if (frame == NULL || out == NULL) {
        return -EINVAL;
    }
    const struct mesh_tern_field *fields = mesh_tern_fields(frame->type, version);
    if (fields == NULL) {
        return -EINVAL;
    }
    if (out_len < 2U) {
        return -ENOSPC;
    }
    if (frame->type == MESH_TERN_INFO) {
        version = mesh_tern_info_version(version, frame->version);
    }
    out[0] = frame->type;
    out[1] = frame->seq;
    size_t at = 2U;
    for (size_t i = 0; i < TERN_FIELDS_MAX && fields[i].kind != TERN_END; ++i) {
        struct mesh_tern_field f = fields[i];
        if (f.since > version) {
            break;
        }
        if (f.kind == TERN_SET_VALUE && !mesh_tern_set_field(frame->setting, version, &f)) {
            return -EINVAL;
        }
        if (f.kind == TERN_STR || f.kind == TERN_BYTES) {
            const size_t n = *((const uint8_t *)frame + f.len_offset);
            if (n > f.max) {
                return -EINVAL;
            }
            if (out_len - at < 1U + n) {
                return -ENOSPC;
            }
            out[at] = (uint8_t)n;
            memcpy(out + at + 1U, (const uint8_t *)frame + f.offset, n);
            at += 1U + n;
            continue;
        }
        const size_t size = mesh_tern_kind_size(f.kind);
        if (out_len - at < size) {
            return -ENOSPC;
        }
        if (f.kind == TERN_ADDR || f.kind == TERN_GID || f.kind == TERN_DIGEST) {
            memcpy(out + at, (const uint8_t *)frame + f.offset, size);
        } else {
            mesh_tern_put_be(out + at, mesh_tern_load_number(frame, &f), size);
        }
        at += size;
    }
    return (int)at;
}

uint32_t mesh_tern_routing_id(const uint8_t address[MESH_TERN_ADDRESS_LEN]) {
    static const char k_label[] = "tern routing id";
    uint8_t digest[INKWELL_SHA256_DIGEST_LEN];
    struct inkwell_sha256 sha;
    inkwell_sha256_init(&sha);
    inkwell_sha256_update(&sha, k_label, sizeof k_label - 1U);
    inkwell_sha256_update(&sha, address, MESH_TERN_ADDRESS_LEN);
    inkwell_sha256_final(&sha, digest);
    for (size_t i = 0; i + 4U <= sizeof digest; i += 4U) {
        const uint32_t word = mesh_tern_be(digest + i, 4U);
        if (word != 0U && word != 0xFFFFFFFFU) {
            return word;
        }
    }
    /* All eight reserved: probability 2^-248. */
    return 0U;
}
