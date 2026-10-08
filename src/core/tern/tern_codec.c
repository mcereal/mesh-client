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
 * the walk picks its value's kind from `setting`.
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
    TERN_U32,
    TERN_ADDR,
    TERN_GID,
    TERN_STR,       /* `max` is the longest it may be */
    TERN_SET_VALUE, /* SET's value, as `setting` says */
};

struct mesh_tern_field {
    uint8_t kind;
    uint8_t max;
    uint16_t offset;
    uint8_t since;
};

#define F_U8(member)                                                                               \
    { TERN_U8, 0U, (uint16_t)offsetof(struct mesh_tern_frame, member), 0U }
#define F_I8(member)                                                                               \
    { TERN_I8, 0U, (uint16_t)offsetof(struct mesh_tern_frame, member), 0U }
#define F_U16(member)                                                                              \
    { TERN_U16, 0U, (uint16_t)offsetof(struct mesh_tern_frame, member), 0U }
#define F_U32(member)                                                                              \
    { TERN_U32, 0U, (uint16_t)offsetof(struct mesh_tern_frame, member), 0U }
#define F_ADDR                                                                                     \
    { TERN_ADDR, 0U, 0U, 0U }
#define F_STR(max)                                                                                 \
    { TERN_STR, (uint8_t)(max), 0U, 0U }
#define F_SET                                                                                      \
    { TERN_SET_VALUE, 0U, 0U, 0U }
#define F_GID                                                                                      \
    { TERN_GID, 0U, 0U, 0U }
/* A message's `wait`: a u16 on the wire, into the u32 member AIRTIME also uses. */
#define F_WAIT                                                                                     \
    { TERN_U16, 0U, (uint16_t)offsetof(struct mesh_tern_frame, wait), 0U }
/* SYNCED's count, which version 3 added. */
#define F_NEWS                                                                                     \
    { TERN_U8, 0U, (uint16_t)offsetof(struct mesh_tern_frame, news), 3U }

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
    {MESH_TERN_OK, 0U, {{0}}},
    {MESH_TERN_ERROR, 0U, {F_U8(code)}},
    {MESH_TERN_INFO, 0U, {F_U8(version), F_STR(MESH_TERN_FIRMWARE_MAX)}},
    {MESH_TERN_SYNCED, 0U, {F_NEWS}},
    {MESH_TERN_QUEUED, 0U, {F_U32(id)}},
    {MESH_TERN_MADE, 2U, {F_GID}},
    {MESH_TERN_SELF,
     0U,
     {F_ADDR, F_U8(role), F_STR(MESH_TERN_REGION_MAX), F_I8(power), F_U32(time)}},
    {MESH_TERN_CONTACT, 0U, {F_ADDR, F_U8(session), F_STR(MESH_TERN_NAME_MAX)}},
    {MESH_TERN_CONTACT_GONE, 0U, {F_ADDR}},
    {MESH_TERN_MESSAGE,
     0U,
     {F_U32(id), F_ADDR, F_U32(time), F_U8(flags), F_U8(state), F_U8(reason), F_WAIT,
      F_STR(MESH_TERN_TEXT_MAX)}},
    {MESH_TERN_STATE, 0U, {F_U32(id), F_U8(state), F_U8(reason), F_WAIT}},
    {MESH_TERN_NEIGHBOUR, 0U, {F_U32(routing_id), F_U8(role), F_I8(snr), F_U16(heard)}},
    {MESH_TERN_NEIGHBOUR_GONE, 0U, {F_U32(routing_id)}},
    {MESH_TERN_AIRTIME, 0U, {F_U32(period), F_U32(allowed), F_U32(used), F_U32(wait)}},
    {MESH_TERN_POWER, 0U, {F_U16(millivolts), F_U8(percent), F_U8(flags)}},
    {MESH_TERN_ASKED, 1U, {F_ADDR, F_U8(why)}},
    {MESH_TERN_GROUP, 2U, {F_GID, F_STR(MESH_TERN_NAME_MAX)}},
    {MESH_TERN_GROUP_GONE, 2U, {F_GID}},
    {MESH_TERN_GROUP_MESSAGE,
     2U,
     {F_U32(id), F_GID, F_U32(from), F_U32(time), F_U8(flags), F_U8(state), F_U8(reason), F_WAIT,
      F_STR(MESH_TERN_TEXT_MAX)}},
    {MESH_TERN_INVITE,
     2U,
     {F_U32(id), F_ADDR, F_GID, F_U32(time), F_U8(flags), F_U8(state), F_U8(reason), F_WAIT,
      F_STR(MESH_TERN_NAME_MAX)}},
};

/* A message's and STATE's `wait` is a u16 on the wire into the u32 member AIRTIME also uses; the
   walk widens or narrows it through the kind, never through the member's size. */

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

/* The kind and member SET's value takes, or false for a setting this version lacks. */
static bool mesh_tern_set_value(uint8_t setting, struct mesh_tern_field *out) {
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
    }
    return false;
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
        return 2U;
    case TERN_U32:
        return 4U;
    case TERN_ADDR:
        return MESH_TERN_ADDRESS_LEN;
    case TERN_GID:
        return MESH_TERN_GROUP_LEN;
    default:
        return 0U;
    }
}

/* One numeric field into its member. A u16 may land in a u32 member (`wait`). */
static void mesh_tern_store_number(struct mesh_tern_frame *out, const struct mesh_tern_field *f,
                                   uint32_t value) {
    uint8_t *member = (uint8_t *)out + f->offset;
    switch (f->kind) {
    case TERN_U8:
        *member = (uint8_t)value;
        break;
    case TERN_I8:
        *(int8_t *)(void *)member = (int8_t)(uint8_t)value;
        break;
    case TERN_U16:
        if (f->offset == offsetof(struct mesh_tern_frame, wait)) {
            out->wait = value;
        } else {
            *(uint16_t *)(void *)member = (uint16_t)value;
        }
        break;
    case TERN_U32:
        *(uint32_t *)(void *)member = value;
        break;
    default:
        break;
    }
}

static uint32_t mesh_tern_load_number(const struct mesh_tern_frame *frame,
                                      const struct mesh_tern_field *f) {
    const uint8_t *member = (const uint8_t *)frame + f->offset;
    switch (f->kind) {
    case TERN_U8:
        return *member;
    case TERN_I8:
        return (uint8_t) * (const int8_t *)(const void *)member;
    case TERN_U16:
        return f->offset == offsetof(struct mesh_tern_frame, wait)
                   ? (frame->wait > 0xFFFFU ? 0xFFFFU : frame->wait)
                   : *(const uint16_t *)(const void *)member;
    case TERN_U32:
        return *(const uint32_t *)(const void *)member;
    default:
        return 0U;
    }
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
    size_t at = 2U;
    for (size_t i = 0; i < TERN_FIELDS_MAX && fields[i].kind != TERN_END; ++i) {
        struct mesh_tern_field f = fields[i];
        if (f.since > version) {
            break; /* a later version's, and so is everything after it */
        }
        if (f.kind == TERN_SET_VALUE && !mesh_tern_set_value(out->setting, &f)) {
            return MESH_TERN_DECODE_UNKNOWN;
        }
        if (f.kind == TERN_STR) {
            if (at >= len) {
                return MESH_TERN_DECODE_MALFORMED;
            }
            const size_t n = frame[at];
            if (n > f.max || len - at - 1U < n) {
                return MESH_TERN_DECODE_MALFORMED;
            }
            if (!mesh_tern_utf8_valid(frame + at + 1U, n)) {
                return MESH_TERN_DECODE_MALFORMED;
            }
            memcpy(out->text, frame + at + 1U, n);
            out->text[n] = '\0';
            out->text_len = (uint8_t)n;
            at += 1U + n;
            continue;
        }
        const size_t size = mesh_tern_kind_size(f.kind);
        if (len - at < size) {
            return MESH_TERN_DECODE_MALFORMED;
        }
        if (f.kind == TERN_ADDR) {
            memcpy(out->address, frame + at, MESH_TERN_ADDRESS_LEN);
        } else if (f.kind == TERN_GID) {
            memcpy(out->group, frame + at, MESH_TERN_GROUP_LEN);
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
    out[0] = frame->type;
    out[1] = frame->seq;
    size_t at = 2U;
    for (size_t i = 0; i < TERN_FIELDS_MAX && fields[i].kind != TERN_END; ++i) {
        struct mesh_tern_field f = fields[i];
        if (f.since > version) {
            break;
        }
        if (f.kind == TERN_SET_VALUE && !mesh_tern_set_value(frame->setting, &f)) {
            return -EINVAL;
        }
        if (f.kind == TERN_STR) {
            const size_t n = frame->text_len;
            if (n > f.max) {
                return -EINVAL;
            }
            if (out_len - at < 1U + n) {
                return -ENOSPC;
            }
            out[at] = (uint8_t)n;
            memcpy(out + at + 1U, frame->text, n);
            at += 1U + n;
            continue;
        }
        const size_t size = mesh_tern_kind_size(f.kind);
        if (out_len - at < size) {
            return -ENOSPC;
        }
        if (f.kind == TERN_ADDR) {
            memcpy(out + at, frame->address, MESH_TERN_ADDRESS_LEN);
        } else if (f.kind == TERN_GID) {
            memcpy(out + at, frame->group, MESH_TERN_GROUP_LEN);
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
