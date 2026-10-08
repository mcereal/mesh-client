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
    {"SEND", MESH_TERN_SEND},
    {"READ", MESH_TERN_READ},
    {"SAVE_CONTACT", MESH_TERN_SAVE_CONTACT},
    {"REMOVE_CONTACT", MESH_TERN_REMOVE_CONTACT},
    {"OK", MESH_TERN_OK},
    {"ERROR", MESH_TERN_ERROR},
    {"INFO", MESH_TERN_INFO},
    {"SYNCED", MESH_TERN_SYNCED},
    {"QUEUED", MESH_TERN_QUEUED},
    {"SELF", MESH_TERN_SELF},
    {"CONTACT", MESH_TERN_CONTACT},
    {"CONTACT_GONE", MESH_TERN_CONTACT_GONE},
    {"MESSAGE", MESH_TERN_MESSAGE},
    {"STATE", MESH_TERN_STATE},
    {"NEIGHBOUR", MESH_TERN_NEIGHBOUR},
    {"NEIGHBOUR_GONE", MESH_TERN_NEIGHBOUR_GONE},
    {"AIRTIME", MESH_TERN_AIRTIME},
    {"POWER", MESH_TERN_POWER},
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

/* One field of a vector's `fields`, into the member struct mesh_tern_frame keeps it in. */
static bool read_field(struct inkwell_json *json, const char *key, struct mesh_tern_frame *f) {
    static const char *const strings[] = {"text", "name", "firmware", "region"};
    static const char *const addresses[] = {"to", "address", "contact"};
    for (size_t i = 0; i < 4U; ++i) {
        if (strcmp(key, strings[i]) == 0 ||
            (strcmp(key, "value") == 0 && i == 0U && f->setting == MESH_TERN_SETTING_REGION)) {
            if (!inkwell_json_read_string(json, f->text, sizeof f->text)) {
                return false;
            }
            f->text_len = (uint8_t)strlen(f->text);
            return true;
        }
    }
    for (size_t i = 0; i < 3U; ++i) {
        if (strcmp(key, addresses[i]) == 0) {
            size_t len = 0U;
            return read_hex(json, f->address, sizeof f->address, &len) &&
                   len == MESH_TERN_ADDRESS_LEN;
        }
    }
    int64_t v = 0;
    if (!read_number(json, &v)) {
        return false;
    }
    if (strcmp(key, "value") == 0) {
        key = f->setting == MESH_TERN_SETTING_ROLE    ? "role"
              : f->setting == MESH_TERN_SETTING_POWER ? "power"
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
    NUMBER(power, int8_t);
    NUMBER(heard, uint16_t);
    NUMBER(millivolts, uint16_t);
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
    uint8_t frame[256];
    size_t frame_len;
    uint8_t stream[256];
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
        const int len = mesh_tern_encode(&v.fields, built, sizeof built);
        struct mesh_tern_frame decoded;
        uint8_t wrapped[MESH_TERN_MAX_FRAME + 6U];
        size_t wrapped_len = 0U;
        struct found found;
        struct mesh_stream_parser parser;
        if (len != (int)v.frame_len || memcmp(built, v.frame, v.frame_len) != 0) {
            snprintf(failure, sizeof failure, "frame case %zu (0x%02x) builds other bytes", cases,
                     v.fields.type);
        } else if (mesh_tern_decode(v.frame, v.frame_len, &decoded) != MESH_TERN_DECODE_OK ||
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
    MESH_TEST_FAIL_IF(cases != 34U, "all 34 frame vectors");
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
             mesh_tern_decode(v.frame, v.frame_len, &decoded) == MESH_TERN_DECODE_OK &&
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
        uint8_t frame[256];
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
        const enum mesh_tern_decode_result result = mesh_tern_decode(frame, len, &decoded);
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
    MESH_TEST_FAIL_IF(cases != 15U, "all 15 rejected vectors");
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
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, out, sizeof out) >= 0,
                      "a name past 31 bytes has no field to go in");
    frame = (struct mesh_tern_frame){.type = MESH_TERN_SET, .setting = 9U};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, out, sizeof out) >= 0,
                      "nor a setting draft 0 lacks");
    frame = (struct mesh_tern_frame){.type = 0x20U};
    MESH_TEST_FAIL_IF(mesh_tern_encode(&frame, out, sizeof out) >= 0, "nor a type it lacks");
    size_t written = 0U;
    MESH_TEST_FAIL_IF(mesh_tern_frame_encode(out, 1U, out, sizeof out, &written) >= 0,
                      "and a byte stream carries no frame shorter than two bytes");
    record_success(test_name);
}
