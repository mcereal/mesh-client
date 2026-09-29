#include "mesh/core/radio_backup_diff.h"

#include "inkwell/base/text.h"

#include <string.h>

void mesh_radio_backup_diff_reset(struct mesh_radio_backup_diff *diff, uint8_t protocol) {
    if (diff == NULL) {
        return;
    }
    memset(diff, 0, sizeof *diff);
    diff->protocol = protocol;
}

struct mesh_radio_backup_change *mesh_radio_backup_diff_add(struct mesh_radio_backup_diff *diff,
                                                            uint8_t kind, uint8_t topic,
                                                            uint16_t index, uint16_t field) {
    if (diff == NULL) {
        return NULL;
    }
    ++diff->total;
    if (diff->count >= MESH_RADIO_BACKUP_DIFF_MAX) {
        return NULL;
    }
    struct mesh_radio_backup_change *change = &diff->changes[diff->count++];
    memset(change, 0, sizeof *change);
    change->kind = kind;
    change->topic = topic;
    change->index = index;
    change->field = field;
    return change;
}

void mesh_radio_backup_value_bool(struct mesh_radio_backup_value *value, bool b) {
    memset(value, 0, sizeof *value);
    value->kind = MESH_RADIO_BACKUP_VALUE_BOOL;
    value->number = b ? 1 : 0;
}

void mesh_radio_backup_value_int(struct mesh_radio_backup_value *value, int64_t number) {
    memset(value, 0, sizeof *value);
    value->kind = MESH_RADIO_BACKUP_VALUE_INT;
    value->number = number;
}

void mesh_radio_backup_value_uint(struct mesh_radio_backup_value *value, uint64_t number) {
    memset(value, 0, sizeof *value);
    value->kind = MESH_RADIO_BACKUP_VALUE_UINT;
    value->number = (int64_t)number;
}

void mesh_radio_backup_value_decimal(struct mesh_radio_backup_value *value, int64_t number,
                                     uint8_t decimals) {
    memset(value, 0, sizeof *value);
    value->kind = MESH_RADIO_BACKUP_VALUE_DECIMAL;
    value->number = number;
    value->decimals = decimals;
}

void mesh_radio_backup_value_text(struct mesh_radio_backup_value *value, const char *text,
                                  size_t len) {
    memset(value, 0, sizeof *value);
    value->kind = MESH_RADIO_BACKUP_VALUE_TEXT;
    if (text != NULL) {
        inkwell_text_sanitise((const uint8_t *)text, strnlen(text, len), value->text,
                              sizeof value->text);
    }
}

void mesh_radio_backup_value_opaque(struct mesh_radio_backup_value *value) {
    memset(value, 0, sizeof *value);
    value->kind = MESH_RADIO_BACKUP_VALUE_OPAQUE;
}
