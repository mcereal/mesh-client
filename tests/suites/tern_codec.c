/*
 * Tern's companion protocol against the specification's own vectors: tests/data/tern_companion.json
 * and tests/data/tern_routing.json are ternmesh/spec's vectors/companion.json and
 * vectors/routing.json, verbatim (CC0). Passing them is what the specification calls
 * "Tern-compatible", so every case reads the file rather than a copy of its numbers.
 */

#include "framework/mesh_test.h"
#include "support/data_fixture.h"

#include "inkwell/codec/json.h"
#include "mesh/core/tern.h"
#include "mesh/proto/stream_framing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *name;
    uint8_t type;
} k_types[] = {
    {"HELLO", MESH_TERN_HELLO},
    {"SYNC", MESH_TERN_SYNC},
    {"PING", MESH_TERN_PING},
    {"SET_TIME", MESH_TERN_SET_TIME},
    {"SET", MESH_TERN_SET},
    {"OFF", MESH_TERN_OFF},
    {"SEND", MESH_TERN_SEND},
    {"READ", MESH_TERN_READ},
    {"SAVE_CONTACT", MESH_TERN_SAVE_CONTACT},
    {"REMOVE_CONTACT", MESH_TERN_REMOVE_CONTACT},
    {"END_SESSION", MESH_TERN_END_SESSION},
    {"MAKE_GROUP", MESH_TERN_MAKE_GROUP},
    {"LEAVE_GROUP", MESH_TERN_LEAVE_GROUP},
    {"NAME_GROUP", MESH_TERN_NAME_GROUP},
    {"SEND_GROUP", MESH_TERN_SEND_GROUP},
    {"SEND_INVITE", MESH_TERN_SEND_INVITE},
    {"JOIN", MESH_TERN_JOIN},
    {"GROUP_LINK", MESH_TERN_GROUP_LINK},
    {"JOIN_LINK", MESH_TERN_JOIN_LINK},
    {"UPDATE_BEGIN", MESH_TERN_UPDATE_BEGIN},
    {"UPDATE_DATA", MESH_TERN_UPDATE_DATA},
    {"UPDATE_END", MESH_TERN_UPDATE_END},
    {"SET_POSITION", MESH_TERN_SET_POSITION},
    {"SHARE", MESH_TERN_SHARE},
    {"SHARE_GROUP", MESH_TERN_SHARE_GROUP},
    {"OK", MESH_TERN_OK},
    {"ERROR", MESH_TERN_ERROR},
    {"INFO", MESH_TERN_INFO},
    {"SYNCED", MESH_TERN_SYNCED},
    {"QUEUED", MESH_TERN_QUEUED},
    {"MADE", MESH_TERN_MADE},
    {"UPDATING", MESH_TERN_UPDATING},
    {"LINK", MESH_TERN_LINK},
    {"SELF", MESH_TERN_SELF},
    {"CONTACT", MESH_TERN_CONTACT},
    {"CONTACT_GONE", MESH_TERN_CONTACT_GONE},
    {"MESSAGE", MESH_TERN_MESSAGE},
    {"STATE", MESH_TERN_STATE},
    {"NEIGHBOUR", MESH_TERN_NEIGHBOUR},
    {"NEIGHBOUR_GONE", MESH_TERN_NEIGHBOUR_GONE},
    {"AIRTIME", MESH_TERN_AIRTIME},
    {"POWER", MESH_TERN_POWER},
    {"ASKED", MESH_TERN_ASKED},
    {"GROUP", MESH_TERN_GROUP},
    {"GROUP_GONE", MESH_TERN_GROUP_GONE},
    {"GROUP_MESSAGE", MESH_TERN_GROUP_MESSAGE},
    {"INVITE", MESH_TERN_INVITE},
    {"POSITION", MESH_TERN_POSITION},
    {"GROUP_POSITION", MESH_TERN_GROUP_POSITION},
    {"SHARING", MESH_TERN_SHARING},
    {"GROUP_SHARING", MESH_TERN_GROUP_SHARING},
    {"CARD", MESH_TERN_CARD},
    {"CARD_GONE", MESH_TERN_CARD_GONE},
    {"FIX", MESH_TERN_FIX},
};

static int type_named(const char *name) {
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; ++i) {
        if (strcmp(k_types[i].name, name) == 0) {
            return k_types[i].type;
        }
    }
    return -1;
}

static size_t unhex(const char *hex, uint8_t *out, size_t out_len) {
    size_t n = 0U;
    while (hex[0] != '\0' && hex[1] != '\0' && n < out_len) {
        char pair[3] = {hex[0], hex[1], '\0'};
        out[n++] = (uint8_t)strtoul(pair, NULL, 16);
        hex += 2;
    }
    return n;
}

/* A number, which the vectors write signed where the field is. */
static bool read_number(struct inkwell_json *json, int64_t *out) {
    while (json->cursor < json->end && (*json->cursor == ' ' || *json->cursor == '\n')) {
        ++json->cursor;
    }
    bool negative = false;
    if (json->cursor < json->end && *json->cursor == '-') {
        negative = true;
        ++json->cursor;
    }
    uint64_t magnitude = 0U;
    if (!inkwell_json_read_u64(json, &magnitude)) {
        return false;
    }
    *out = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    return true;
}

static bool read_hex(struct inkwell_json *json, uint8_t *out, size_t out_len, size_t *len) {
    char hex[1024];
    if (!inkwell_json_read_string(json, hex, sizeof hex)) {
        return false;
    }
    *len = unhex(hex, out, out_len);
    return true;
}

/* A string of the vectors into one of the frame's string members and its length. */
static bool read_string(struct inkwell_json *json, char *out, size_t out_len, uint8_t *len) {
    if (!inkwell_json_read_string(json, out, out_len)) {
        return false;
    }
    *len = (uint8_t)strlen(out);
    return true;
}

/* One field of a vector's `fields`, into the member struct mesh_tern_frame keeps it in. */
static bool read_field(struct inkwell_json *json, const char *key, struct mesh_tern_frame *f) {
    static const char *const strings[] = {"text", "name", "firmware", "region", "link"};
    static const char *const addresses[] = {"to", "address", "contact"};
    const bool string_value =
        strcmp(key, "value") == 0 &&
        (f->setting == MESH_TERN_SETTING_REGION || f->setting == MESH_TERN_SETTING_CARD_NAME);
    for (size_t i = 0; i < sizeof strings / sizeof strings[0]; ++i) {
        if (strcmp(key, strings[i]) == 0 || (i == 0U && string_value)) {
            return read_string(json, f->text, sizeof f->text, &f->text_len);
        }
    }
    if (strcmp(key, "board") == 0) {
        return read_string(json, f->board, sizeof f->board, &f->board_len);
    }
    if (strcmp(key, "release") == 0) {
        return read_string(json, f->release, sizeof f->release, &f->release_len);
    }
    if (strcmp(key, "card_name") == 0) {
        return read_string(json, f->card_name, sizeof f->card_name, &f->card_name_len);
    }
    for (size_t i = 0; i < sizeof addresses / sizeof addresses[0]; ++i) {
        if (strcmp(key, addresses[i]) == 0) {
            size_t len = 0U;
            return read_hex(json, f->address, sizeof f->address, &len) &&
                   len == MESH_TERN_ADDRESS_LEN;
        }
    }
    if (strcmp(key, "group") == 0) {
        size_t len = 0U;
        return read_hex(json, f->group, sizeof f->group, &len) && len == MESH_TERN_GROUP_LEN;
    }
    if (strcmp(key, "digest") == 0) {
        size_t len = 0U;
        return read_hex(json, f->digest, sizeof f->digest, &len) && len == MESH_TERN_DIGEST_LEN;
    }
    if (strcmp(key, "data") == 0) {
        size_t len = 0U;
        const bool ok = read_hex(json, f->data, sizeof f->data, &len);
        f->data_len = (uint8_t)len;
        return ok;
    }
    int64_t v = 0;
    if (!read_number(json, &v)) {
        return false;
    }
    if (strcmp(key, "value") == 0) {
        key = f->setting == MESH_TERN_SETTING_ROLE       ? "role"
              : f->setting == MESH_TERN_SETTING_POWER    ? "power"
              : f->setting == MESH_TERN_SETTING_CARDS    ? "cards"
              : f->setting == MESH_TERN_SETTING_RECEIVER ? "receiver"
                                                         : "passkey";
    }
#define NUMBER(member, cast)                                                                       \
    do {                                                                                           \
        if (strcmp(key, #member) == 0) {                                                           \
            f->member = (cast)v;                                                                   \
            return true;                                                                           \
        }                                                                                          \
    } while (0)
    NUMBER(version, uint8_t);
    NUMBER(setting, uint8_t);
    NUMBER(code, uint8_t);
    NUMBER(role, uint8_t);
    NUMBER(session, uint8_t);
    NUMBER(flags, uint8_t);
    NUMBER(state, uint8_t);
    NUMBER(reason, uint8_t);
    NUMBER(percent, uint8_t);
    NUMBER(why, uint8_t);
    NUMBER(news, uint8_t);
    NUMBER(cards, uint8_t);
    NUMBER(precision, uint8_t);
    NUMBER(fields, uint8_t);
    NUMBER(receiver, uint8_t);
    NUMBER(source, uint8_t);
    NUMBER(satellites, uint8_t);
    NUMBER(power, int8_t);
    NUMBER(altitude, int16_t);
    NUMBER(accuracy, uint16_t);
    NUMBER(interval, uint16_t);
    NUMBER(minutes, uint16_t);
    NUMBER(millivolts, uint16_t);
    NUMBER(lat, int32_t);
    NUMBER(lon, int32_t);
    NUMBER(age, uint32_t);
    NUMBER(heard, uint32_t);
    NUMBER(size, uint32_t);
    NUMBER(offset, uint32_t);
    NUMBER(after, uint32_t);
    NUMBER(time, uint32_t);
    NUMBER(ref, uint32_t);
    NUMBER(through, uint32_t);
    NUMBER(id, uint32_t);
    NUMBER(routing_id, uint32_t);
    NUMBER(period, uint32_t);
    NUMBER(allowed, uint32_t);
    NUMBER(used, uint32_t);
    NUMBER(wait, uint32_t);
    NUMBER(passkey, uint32_t);
    NUMBER(from, uint32_t);
#undef NUMBER
    if (strcmp(key, "snr_quarter_db") == 0) {
        f->snr = (int8_t)v;
        return true;
    }
    return false;
}

/* What a case in `frames` or `extended` says: the fields as a struct, the frame, the stream. */
struct vector {
    struct mesh_tern_frame fields;
    uint8_t frame[MESH_TERN_MAX_FRAME + 8U];
    size_t frame_len;
    uint8_t stream[MESH_TERN_MAX_FRAME + 8U];
    size_t stream_len;
};

static bool read_vector(struct inkwell_json *json, struct vector *v) {
    memset(v, 0, sizeof *v);
    if (!inkwell_json_enter_object(json)) {
        return false;
    }
    char key[32];
    while (inkwell_json_next_key(json, key, sizeof key)) {
        bool ok = true;
        if (strcmp(key, "type") == 0) {
            char name[32];
            ok = inkwell_json_read_string(json, name, sizeof name);
            const int type = type_named(name);
            ok = ok && type >= 0;
            v->fields.type = (uint8_t)type;
        } else if (strcmp(key, "seq") == 0) {
            int64_t seq = 0;
            ok = read_number(json, &seq);
            v->fields.seq = (uint8_t)seq;
        } else if (strcmp(key, "fields") == 0) {
            ok = inkwell_json_enter_object(json);
            char field[32];
            while (ok && inkwell_json_next_key(json, field, sizeof field)) {
                ok = read_field(json, field, &v->fields);
            }
        } else if (strcmp(key, "frame") == 0) {
            ok = read_hex(json, v->frame, sizeof v->frame, &v->frame_len);
        } else if (strcmp(key, "stream") == 0) {
            ok = read_hex(json, v->stream, sizeof v->stream, &v->stream_len);
        } else {
            ok = inkwell_json_skip_value(json);
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* The document, with the cursor on the array under `section`. */
static char *open_section(const char *file, const char *section, struct inkwell_json *json) {
    size_t len = 0U;
    char *document = mesh_test_data_read(file, &len);
    if (document == NULL) {
        return NULL;
    }
    inkwell_json_init(json, document, len);
    if (!inkwell_json_object_find(json, section) || !inkwell_json_enter_array(json)) {
        free(document);
        return NULL;
    }
    return document;
}

MESH_TEST_CASE(tern_crc_is_ibm_3740, unit) {
    struct inkwell_json json;
    size_t len = 0U;
    char *document = mesh_test_data_read("tern_companion.json", &len);
    MESH_TEST_FAIL_IF(document == NULL, "the vectors should be there");
    inkwell_json_init(&json, document, len);
    uint8_t input[32];
    size_t input_len = 0U;
    uint64_t crc = 0U;
    char key[16];
    const bool read = inkwell_json_object_find(&json, "crc_check") &&
                      inkwell_json_object_find(&json, "input") &&
                      read_hex(&json, input, sizeof input, &input_len) &&
                      inkwell_json_next_key(&json, key, sizeof key) && strcmp(key, "crc") == 0 &&
                      inkwell_json_read_u64(&json, &crc);
    free(document);
    MESH_TEST_FAIL_IF(!read, "crc_check should read");
    MESH_TEST_FAIL_IF(mesh_tern_crc16(input, input_len) != (uint16_t)crc,
                      "the check value over \"123456789\"");
    record_success(test_name);
}

/* What a parser found, with runs of text joined as the vectors write them. */
struct found {
    uint8_t items[16][256];
    size_t lens[16];
    bool is_text[16];
    size_t count;
};

static void found_frame(const uint8_t *payload, size_t len, void *ctx) {
    struct found *found = ctx;
    if (found->count < 16U) {
        memcpy(found->items[found->count], payload, len);
        found->lens[found->count] = len;
        found->is_text[found->count] = false;
        found->count += 1U;
    }
}

static void found_text(const uint8_t *text, size_t len, void *ctx) {
    struct found *found = ctx;
    if (found->count > 0U && found->is_text[found->count - 1U]) {
        size_t *at = &found->lens[found->count - 1U];
        memcpy(found->items[found->count - 1U] + *at, text, len);
        *at += len;
        return;
    }
    if (found->count < 16U) {
        memcpy(found->items[found->count], text, len);
        found->lens[found->count] = len;
        found->is_text[found->count] = true;
        found->count += 1U;
    }
}

static void parse(const uint8_t *stream, size_t len, bool bytewise, struct found *found,
                  struct mesh_stream_parser *parser) {
    memset(found, 0, sizeof *found);
    mesh_stream_parser_reset(parser);
    const struct mesh_stream_parser_callbacks callbacks = {
        .on_frame = found_frame, .on_text = found_text, .ctx = found};
    if (bytewise) {
        for (size_t i = 0; i < len; ++i) {
            mesh_stream_framing_tern.push(parser, stream + i, 1U, &callbacks);
        }
    } else {
        mesh_stream_framing_tern.push(parser, stream, len, &callbacks);
    }
}

MESH_TEST_CASE(tern_frames_build_read_and_wrap_as_the_vectors_say, unit) {
    struct inkwell_json json;
    char *document = open_section("tern_companion.json", "frames", &json);
    MESH_TEST_FAIL_IF(document == NULL, "the frames should be there");
    size_t cases = 0U;
    char failure[160] = "";
    while (failure[0] == '\0' && inkwell_json_next_element(&json)) {
        struct vector v;
        if (!read_vector(&json, &v)) {
            snprintf(failure, sizeof failure, "frame case %zu should read", cases);
            break;
        }
        uint8_t built[MESH_TERN_MAX_FRAME];
        const int len = mesh_tern_encode(&v.fields, MESH_TERN_VERSION, built, sizeof built);
        struct mesh_tern_frame decoded;
        uint8_t wrapped[MESH_TERN_MAX_FRAME + 6U];
        size_t wrapped_len = 0U;
        struct found found;
        struct mesh_stream_parser parser;
        if (len != (int)v.frame_len || memcmp(built, v.frame, v.frame_len) != 0) {
            snprintf(failure, sizeof failure, "frame case %zu (0x%02x) builds other bytes", cases,
                     v.fields.type);
        } else if (mesh_tern_decode(v.frame, v.frame_len, MESH_TERN_VERSION, &decoded) !=
                       MESH_TERN_DECODE_OK ||
                   memcmp(&decoded, &v.fields, sizeof decoded) != 0) {
            snprintf(failure, sizeof failure, "frame case %zu (0x%02x) reads other fields", cases,
                     v.fields.type);
        } else if (mesh_tern_frame_encode(v.frame, v.frame_len, wrapped, sizeof wrapped,
                                          &wrapped_len) != 0 ||
                   wrapped_len != v.stream_len || memcmp(wrapped, v.stream, v.stream_len) != 0) {
            snprintf(failure, sizeof failure, "frame case %zu wraps to another stream", cases);
        } else {
            parse(v.stream, v.stream_len, false, &found, &parser);
            if (found.count != 1U || found.is_text[0] || found.lens[0] != v.frame_len ||
                memcmp(found.items[0], v.frame, v.frame_len) != 0) {
                snprintf(failure, sizeof failure, "frame case %zu is not found in its stream",
                         cases);
            }
        }
        cases += 1U;
    }
    free(document);
    MESH_TEST_FAIL_IF(failure[0] != '\0', failure);
    MESH_TEST_FAIL_IF(cases != 101U, "all 101 frame vectors");
    record_success(test_name);
}

MESH_TEST_CASE(tern_reads_past_fields_it_does_not_know, unit) {
    struct inkwell_json json;
    char *document = open_section("tern_companion.json", "extended", &json);
    MESH_TEST_FAIL_IF(document == NULL, "the extended cases should be there");
    size_t cases = 0U;
    bool ok = true;
    while (ok && inkwell_json_next_element(&json)) {
        struct vector v;
        struct mesh_tern_frame decoded;
        ok = read_vector(&json, &v) &&
             mesh_tern_decode(v.frame, v.frame_len, MESH_TERN_VERSION, &decoded) ==
                 MESH_TERN_DECODE_OK &&
             memcmp(&decoded, &v.fields, sizeof decoded) == 0;
        cases += 1U;
    }
    free(document);
    MESH_TEST_FAIL_IF(!ok, "a later version's extra bytes are ignored, the fields still read");
    MESH_TEST_FAIL_IF(cases != 2U, "both extended vectors");
    record_success(test_name);
}

MESH_TEST_CASE(tern_rejects_what_the_vectors_reject, unit) {
    struct inkwell_json json;
    char *document = open_section("tern_companion.json", "rejected", &json);
    MESH_TEST_FAIL_IF(document == NULL, "the rejected cases should be there");
    size_t cases = 0U;
    char failure[160] = "";
    while (failure[0] == '\0' && inkwell_json_next_element(&json)) {
        uint8_t frame[MESH_TERN_MAX_FRAME + 8U];
        size_t len = 0U;
        bool answered = false;
        uint64_t answer = 0U;
        char key[32];
        bool ok = inkwell_json_enter_object(&json);
        while (ok && inkwell_json_next_key(&json, key, sizeof key)) {
            if (strcmp(key, "frame") == 0) {
                ok = read_hex(&json, frame, sizeof frame, &len);
            } else if (strcmp(key, "answer") == 0) {
                answered = inkwell_json_read_u64(&json, &answer);
                ok = answered || inkwell_json_skip_value(&json);
            } else {
                ok = inkwell_json_skip_value(&json);
            }
        }
        struct mesh_tern_frame decoded;
        const enum mesh_tern_decode_result result =
            mesh_tern_decode(frame, len, MESH_TERN_VERSION, &decoded);
        if (!ok) {
            snprintf(failure, sizeof failure, "rejected case %zu should read", cases);
        } else if (result == MESH_TERN_DECODE_OK) {
            snprintf(failure, sizeof failure, "rejected case %zu was read", cases);
        } else if (answered && (!MESH_TERN_IS_REQUEST(frame[0]) || (uint64_t)result != answer)) {
            snprintf(failure, sizeof failure, "rejected case %zu: error %d, not %u", cases,
                     (int)result, (unsigned)answer);
        } else if (!answered && result != MESH_TERN_DECODE_SHORT &&
                   MESH_TERN_IS_REQUEST(frame[0])) {
            snprintf(failure, sizeof failure, "rejected case %zu is a request a node answers",
                     cases);
        }
        cases += 1U;
    }
    free(document);
    MESH_TEST_FAIL_IF(failure[0] != '\0', failure);
    MESH_TEST_FAIL_IF(cases != 44U, "all 44 rejected vectors");
    record_success(test_name);
}

MESH_TEST_CASE(tern_finds_frames_and_console_text_in_a_stream, unit) {
    struct inkwell_json json;
    char *document = open_section("tern_companion.json", "streams", &json);
    MESH_TEST_FAIL_IF(document == NULL, "the stream cases should be there");
    size_t cases = 0U;
    char failure[160] = "";
    while (failure[0] == '\0' && inkwell_json_next_element(&json)) {
        uint8_t stream[256];
        size_t stream_len = 0U;
        struct found want;
        memset(&want, 0, sizeof want);
        uint8_t pending[64];
        size_t pending_len = 0U;
        char key[32];
        bool ok = inkwell_json_enter_object(&json);
        while (ok && inkwell_json_next_key(&json, key, sizeof key)) {
            if (strcmp(key, "stream") == 0) {
                ok = read_hex(&json, stream, sizeof stream, &stream_len);
            } else if (strcmp(key, "pending") == 0) {
                ok = read_hex(&json, pending, sizeof pending, &pending_len);
            } else if (strcmp(key, "items") == 0) {
                ok = inkwell_json_enter_array(&json);
                while (ok && inkwell_json_next_element(&json)) {
                    char kind[16];
                    ok = inkwell_json_enter_object(&json) &&
                         inkwell_json_next_key(&json, kind, sizeof kind) && want.count < 16U &&
                         read_hex(&json, want.items[want.count], sizeof want.items[0],
                                  &want.lens[want.count]);
                    want.is_text[want.count] = strcmp(kind, "text") == 0;
                    want.count += 1U;
                    ok = ok && !inkwell_json_next_key(&json, kind, sizeof kind);
                }
            } else {
                ok = inkwell_json_skip_value(&json);
            }
        }
        if (!ok) {
            snprintf(failure, sizeof failure, "stream case %zu should read", cases);
            break;
        }
        for (int bytewise = 0; bytewise <= 1 && failure[0] == '\0'; ++bytewise) {
            struct found found;
            struct mesh_stream_parser parser;
            parse(stream, stream_len, bytewise != 0, &found, &parser);
            bool same = found.count == want.count && parser.len == pending_len &&
                        memcmp(parser.buffer, pending, pending_len) == 0;
            for (size_t i = 0; same && i < found.count; ++i) {
                same = found.is_text[i] == want.is_text[i] && found.lens[i] == want.lens[i] &&
                       memcmp(found.items[i], want.items[i], want.lens[i]) == 0;
            }
            if (!same) {
                snprintf(failure, sizeof failure, "stream case %zu, fed %s, finds otherwise", cases,
                         bytewise ? "a byte at a time" : "whole");
            }
        }
        cases += 1U;
    }
    free(document);
    MESH_TEST_FAIL_IF(failure[0] != '\0', failure);
    MESH_TEST_FAIL_IF(cases != 11U, "all 11 stream vectors");
    record_success(test_name);
}

MESH_TEST_CASE(tern_routing_ids_match_the_routing_vectors, unit) {
    struct inkwell_json json;
    char *document = open_section("tern_routing.json", "ids", &json);
    MESH_TEST_FAIL_IF(document == NULL, "the routing vectors should be there");
    size_t cases = 0U;
    bool ok = true;
    while (ok && inkwell_json_next_element(&json)) {
        uint8_t address[MESH_TERN_ADDRESS_LEN];
        size_t len = 0U;
        uint64_t id = 0U;
        char key[8];
        ok = inkwell_json_object_find(&json, "address") &&
             read_hex(&json, address, sizeof address, &len) && len == sizeof address &&
             inkwell_json_next_key(&json, key, sizeof key) && strcmp(key, "id") == 0 &&
             inkwell_json_read_u64(&json, &id) && !inkwell_json_next_key(&json, key, sizeof key) &&
             mesh_tern_routing_id(address) == (uint32_t)id;
        cases += 1U;
    }
    free(document);
    MESH_TEST_FAIL_IF(!ok || cases != 4U, "each address hashes to the routing id the spec gives");
    record_success(test_name);
}

MESH_TEST_CASE(tern_refuses_to_write_what_no_frame_holds, unit) {
    uint8_t out[MESH_TERN_MAX_FRAME];
    struct mesh_tern_frame frame = {.type = MESH_TERN_SAVE_CONTACT, .text_len = 32U};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, MESH_TERN_VERSION, out, sizeof out) >= 0,
                      "a name past 31 bytes has no field to go in");
    frame = (struct mesh_tern_frame){.type = MESH_TERN_SET, .setting = 9U};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, MESH_TERN_VERSION, out, sizeof out) >= 0,
                      "nor a setting version 9 lacks");
    frame = (struct mesh_tern_frame){.type = MESH_TERN_SET, .setting = MESH_TERN_SETTING_CARDS};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, 5U, out, sizeof out) >= 0,
                      "nor one version 5 lacks");
    frame = (struct mesh_tern_frame){.type = MESH_TERN_SET, .setting = MESH_TERN_SETTING_RECEIVER};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, 7U, out, sizeof out) >= 0 ||
                          mesh_tern_encode(&frame, 8U, out, sizeof out) != 4,
                      "the receiver is version 8's setting");
    frame = (struct mesh_tern_frame){.type = MESH_TERN_OFF};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, 8U, out, sizeof out) >= 0 ||
                          mesh_tern_encode(&frame, 9U, out, sizeof out) != 2,
                      "and OFF version 9's request, two bytes and no field");
    frame = (struct mesh_tern_frame){.type = 0x28U};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, MESH_TERN_VERSION, out, sizeof out) >= 0,
                      "nor a type it lacks");
    frame = (struct mesh_tern_frame){.type = MESH_TERN_UPDATE_DATA, .data_len = 173U};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, MESH_TERN_VERSION, out, sizeof out) >= 0,
                      "nor bytes past UPDATE_CHUNK");
    frame = (struct mesh_tern_frame){.type = MESH_TERN_MAKE_GROUP};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, 1U, out, sizeof out) >= 0,
                      "nor a type a version before it lacks");
    frame = (struct mesh_tern_frame){.type = MESH_TERN_SYNCED, .news = 6U};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, 2U, out, sizeof out) != 2 ||
                          mesh_tern_encode(&frame, 3U, out, sizeof out) != 3,
                      "and SYNCED carries its count from version 3");
    size_t written = 0U;
    MESH_TEST_FAIL_IF(mesh_tern_frame_encode(out, 1U, out, sizeof out, &written) >= 0,
                      "and a byte stream carries no frame shorter than two bytes");
    record_success(test_name);
}

/*
 * Each older connection's frames, read by the version its client speaks: every one reads, and
 * writes back to the same bytes. The SYNCEDs of versions 0 to 2 are two bytes, which version 3
 * would take for cut short, and the INFO of a version 7 node is read without its board and
 * release by a client of version 3 or earlier.
 */
MESH_TEST_CASE(tern_older_connections_read_by_their_own_version, unit) {
    struct inkwell_json json;
    char *document = open_section("tern_companion.json", "older", &json);
    MESH_TEST_FAIL_IF(document == NULL, "the older connections should be there");
    size_t connections = 0U;
    size_t frames = 0U;
    char failure[160] = "";
    while (failure[0] == '\0' && inkwell_json_next_element(&json)) {
        uint64_t version = 99U;
        char key[16];
        bool ok = inkwell_json_enter_object(&json);
        while (ok && failure[0] == '\0' && inkwell_json_next_key(&json, key, sizeof key)) {
            if (strcmp(key, "version") == 0) {
                ok = inkwell_json_read_u64(&json, &version);
                continue;
            }
            if (strcmp(key, "frames") != 0) {
                ok = inkwell_json_skip_value(&json);
                continue;
            }
            ok = version <= MESH_TERN_VERSION && inkwell_json_enter_array(&json);
            while (ok && failure[0] == '\0' && inkwell_json_next_element(&json)) {
                uint8_t frame[MESH_TERN_MAX_FRAME + 8U];
                size_t len = 0U;
                ok = inkwell_json_object_find(&json, "frame") &&
                     read_hex(&json, frame, sizeof frame, &len);
                while (ok && inkwell_json_next_key(&json, key, sizeof key)) {
                    ok = inkwell_json_skip_value(&json);
                }
                struct mesh_tern_frame decoded;
                uint8_t built[MESH_TERN_MAX_FRAME];
                if (!ok) {
                    break;
                }
                if (mesh_tern_frame_since(frame, len) > version) {
                    /* The request each client's version lacks, which it should not have sent:
                       ERROR 1. */
                    if (mesh_tern_decode(frame, len, (uint8_t)version, &decoded) !=
                        MESH_TERN_DECODE_UNKNOWN) {
                        snprintf(failure, sizeof failure, "version %u reads 0x%02x",
                                 (unsigned)version, frame[0]);
                    }
                } else if (mesh_tern_decode(frame, len, (uint8_t)version, &decoded) !=
                           MESH_TERN_DECODE_OK) {
                    snprintf(failure, sizeof failure, "version %u does not read 0x%02x",
                             (unsigned)version, frame[0]);
                } else if (mesh_tern_encode(&decoded, (uint8_t)version, built, sizeof built) !=
                               (int)len ||
                           memcmp(built, frame, len) != 0) {
                    snprintf(failure, sizeof failure, "version %u writes 0x%02x otherwise",
                             (unsigned)version, frame[0]);
                }
                frames += 1U;
            }
        }
        if (!ok && failure[0] == '\0') {
            snprintf(failure, sizeof failure, "older connection %zu should read", connections);
        }
        connections += 1U;
    }
    free(document);
    MESH_TEST_FAIL_IF(failure[0] != '\0', failure);
    MESH_TEST_FAIL_IF(connections != 9U || frames != 115U, "nine connections, 115 frames");
    record_success(test_name);
}

MESH_TEST_CASE(tern_a_type_later_than_the_version_is_unknown_however_it_reads, unit) {
    /* A GROUP cut short after two bytes of its id: malformed at version 2, but at version 1 the
       type is not one it defines, which is the answer before any field is looked at. */
    static const uint8_t cut_group[] = {MESH_TERN_GROUP, 0x10, 0xc0, 0x03};
    static const uint8_t cut_asked[] = {MESH_TERN_ASKED, 0x0e, 0xfc, 0x51};
    static const uint8_t join[] = {MESH_TERN_JOIN, 0x16, 0x00, 0x00, 0x00, 0x16};
    struct mesh_tern_frame decoded;
    MESH_TEST_FAIL_IF(mesh_tern_decode(cut_group, sizeof cut_group, 2U, &decoded) !=
                          MESH_TERN_DECODE_MALFORMED,
                      "cut short where the type is defined");
    MESH_TEST_FAIL_IF(mesh_tern_decode(cut_group, sizeof cut_group, 1U, &decoded) !=
                          MESH_TERN_DECODE_UNKNOWN,
                      "unknown where it is not");
    MESH_TEST_FAIL_IF(mesh_tern_decode(cut_asked, sizeof cut_asked, 0U, &decoded) !=
                              MESH_TERN_DECODE_UNKNOWN ||
                          mesh_tern_decode(cut_asked, sizeof cut_asked, 1U, &decoded) !=
                              MESH_TERN_DECODE_MALFORMED,
                      "ASKED is version 1's");
    MESH_TEST_FAIL_IF(mesh_tern_decode(join, sizeof join, 1U, &decoded) != MESH_TERN_DECODE_UNKNOWN,
                      "a node of version 1 answers JOIN with ERROR 1");
    static const uint8_t synced_v2[] = {MESH_TERN_SYNCED, 0x02};
    static const uint8_t synced_v3[] = {MESH_TERN_SYNCED, 0x02, 0x06};
    MESH_TEST_FAIL_IF(mesh_tern_decode(synced_v2, sizeof synced_v2, 2U, &decoded) !=
                          MESH_TERN_DECODE_OK,
                      "version 2's SYNCED is two bytes");
    MESH_TEST_FAIL_IF(mesh_tern_decode(synced_v3, sizeof synced_v3, 2U, &decoded) !=
                              MESH_TERN_DECODE_OK ||
                          decoded.news != 0U,
                      "and version 2 reads no count from a longer one");
    MESH_TEST_FAIL_IF(mesh_tern_decode(synced_v3, sizeof synced_v3, 3U, &decoded) !=
                              MESH_TERN_DECODE_OK ||
                          decoded.news != 6U,
                      "where version 3 does");
    record_success(test_name);
}

/* A frame of a type, or naming a setting, a version does not define: unknown at that version
   however it reads, which a node answers with ERROR 1 and a client ignores or discards. */
MESH_TEST_CASE(tern_older_versions_take_later_frames_as_unknown, unit) {
    struct inkwell_json json;
    char *document = open_section("tern_companion.json", "unknown_to_older", &json);
    MESH_TEST_FAIL_IF(document == NULL, "the unknown_to_older cases should be there");
    size_t cases = 0U;
    char failure[160] = "";
    while (failure[0] == '\0' && inkwell_json_next_element(&json)) {
        uint8_t frame[MESH_TERN_MAX_FRAME + 8U];
        size_t len = 0U;
        uint64_t version = 99U;
        bool answered = false;
        uint64_t answer = 0U;
        char key[32];
        bool ok = inkwell_json_enter_object(&json);
        while (ok && inkwell_json_next_key(&json, key, sizeof key)) {
            if (strcmp(key, "frame") == 0) {
                ok = read_hex(&json, frame, sizeof frame, &len);
            } else if (strcmp(key, "version") == 0) {
                ok = inkwell_json_read_u64(&json, &version);
            } else if (strcmp(key, "answer") == 0) {
                answered = inkwell_json_read_u64(&json, &answer);
                ok = answered || inkwell_json_skip_value(&json);
            } else {
                ok = inkwell_json_skip_value(&json);
            }
        }
        struct mesh_tern_frame decoded;
        if (!ok || version >= MESH_TERN_VERSION) {
            snprintf(failure, sizeof failure, "unknown_to_older case %zu should read", cases);
        } else if (mesh_tern_decode(frame, len, (uint8_t)version, &decoded) !=
                   MESH_TERN_DECODE_UNKNOWN) {
            snprintf(failure, sizeof failure, "case %zu: version %u reads 0x%02x", cases,
                     (unsigned)version, frame[0]);
        } else if (mesh_tern_decode(frame, len, MESH_TERN_VERSION, &decoded) !=
                   MESH_TERN_DECODE_OK) {
            snprintf(failure, sizeof failure, "case %zu: version %u does not read 0x%02x", cases,
                     (unsigned)MESH_TERN_VERSION, frame[0]);
        } else if (answered != MESH_TERN_IS_REQUEST(frame[0]) ||
                   (answered && answer != MESH_TERN_ERR_UNKNOWN) ||
                   mesh_tern_frame_since(frame, len) <= version) {
            snprintf(failure, sizeof failure, "case %zu: a request is ERROR 1, the rest unanswered",
                     cases);
        }
        cases += 1U;
    }
    free(document);
    MESH_TEST_FAIL_IF(failure[0] != '\0', failure);
    MESH_TEST_FAIL_IF(cases != 21U, "all 21 unknown_to_older vectors");
    record_success(test_name);
}

MESH_TEST_CASE(tern_a_number_wider_in_one_frame_is_written_clamped_to_the_narrower, unit) {
    uint8_t out[MESH_TERN_MAX_FRAME];
    /* A card heard a day ago fits a CARD's four bytes; a neighbour's two say 0xFFFF. */
    struct mesh_tern_frame frame = {.type = MESH_TERN_NEIGHBOUR, .heard = 86399U};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, MESH_TERN_VERSION, out, sizeof out) != 10 ||
                          out[8] != 0xFFU || out[9] != 0xFFU,
                      "a NEIGHBOUR's heard is at most 0xFFFF");
    frame = (struct mesh_tern_frame){.type = MESH_TERN_POSITION, .accuracy = 300U, .age = 1U};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, MESH_TERN_VERSION, out, sizeof out) != 50 ||
                          out[45] != 0xFFU,
                      "and a POSITION's accuracy at most 255");
    frame = (struct mesh_tern_frame){.type = MESH_TERN_FIX, .accuracy = 300U, .age = 70000U};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, MESH_TERN_VERSION, out, sizeof out) != 21 ||
                          out[14] != 0x01U || out[15] != 0x2CU || out[17] != 0x01U ||
                          out[18] != 0x11U || out[19] != 0x70U,
                      "where a FIX carries accuracy in two bytes and age in four");
    struct mesh_tern_frame decoded;
    static const uint8_t altitude[] = {
        MESH_TERN_SET_POSITION, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x00, 0, 4, 0, 3};
    MESH_TEST_FAIL_IF(mesh_tern_decode(altitude, sizeof altitude, MESH_TERN_VERSION, &decoded) !=
                              MESH_TERN_DECODE_OK ||
                          decoded.altitude != MESH_TERN_NO_ALTITUDE || decoded.accuracy != 4U ||
                          decoded.age != 3U,
                      "an i16 reads signed: -32768 is no altitude");
    /* A node of version 7 writes INFO's board and release; a client of version 3 reads its
       INFO by 3, and one of 7 the INFO of a version 3 node by 3. */
    frame = (struct mesh_tern_frame){.type = MESH_TERN_INFO,
                                     .version = 3U,
                                     .text_len = 4U,
                                     .text = "tern",
                                     .board_len = 2U,
                                     .board = "hx"};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, MESH_TERN_VERSION, out, sizeof out) != 8,
                      "INFO is written by the version it carries, when that is less");
    record_success(test_name);
}
