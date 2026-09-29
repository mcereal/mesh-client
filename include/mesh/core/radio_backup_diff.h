#pragma once

/*
 * What differs between two backups of one radio - in practice, a backup on the card and the
 * radio as it is now, captured into a second backup for the purpose.
 *
 * **Two backups rather than a backup and a radio.** Comparing against the live radio means
 * reading it the way a backup does, and the capture already does that: so a comparison is always
 * between two containers, each protocol's diff reads its own sections out of both, and the same
 * call answers "what changed between Tuesday and today" the day a screen asks it.
 *
 * **The list says what, not how to say it.** A change names a topic (the part of a radio: its
 * LoRa settings, a channel slot, a contact), a field inside it by the protocol's own number, and
 * the two values as numbers or text. The words are the screen's: a topic is a heading and a
 * field is a label out of a table keyed on (protocol, topic, field), with the field number
 * itself as the fallback, so a field a newer firmware adds is still listed the day it appears
 * rather than silently compared away.
 *
 * **In the order a reader wants them**: the radio's settings first, then its channels, then -
 * on MeshCore - its contacts, each group in the order the backup holds it. A MeshCore radio can
 * gain a hundred contacts in a week, and a list that led with them would push the one setting
 * that changed off the end. The list is bounded; `total` is how many there were in all.
 *
 * `before` is the first backup's value and `after` the second's. ADDED is a thing only the second
 * has and REMOVED one only the first has, with the value that is missing left ABSENT.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_RADIO_BACKUP_DIFF_MAX 48U
#define MESH_RADIO_BACKUP_VALUE_TEXT_MAX 40U
#define MESH_RADIO_BACKUP_SUBJECT 33U

/* The part of a radio a change is in. Written into nothing, so the numbers are free to move. */
enum mesh_radio_backup_topic {
    MESH_RADIO_BACKUP_TOPIC_NONE = 0,
    MESH_RADIO_BACKUP_TOPIC_DEVICE,
    MESH_RADIO_BACKUP_TOPIC_POSITION,
    MESH_RADIO_BACKUP_TOPIC_POWER,
    MESH_RADIO_BACKUP_TOPIC_NETWORK,
    MESH_RADIO_BACKUP_TOPIC_DISPLAY,
    MESH_RADIO_BACKUP_TOPIC_LORA,
    MESH_RADIO_BACKUP_TOPIC_BLUETOOTH,
    MESH_RADIO_BACKUP_TOPIC_SECURITY,
    MESH_RADIO_BACKUP_TOPIC_MODULE,  /* `index` is the protocol's number for the module */
    MESH_RADIO_BACKUP_TOPIC_CHANNEL, /* `index` is the slot */
    MESH_RADIO_BACKUP_TOPIC_OWNER,   /* who the radio says it is: its names */
    MESH_RADIO_BACKUP_TOPIC_RADIO_UI,
    MESH_RADIO_BACKUP_TOPIC_CANNED,
    MESH_RADIO_BACKUP_TOPIC_RINGTONE,
    MESH_RADIO_BACKUP_TOPIC_FIXED_POSITION,
    MESH_RADIO_BACKUP_TOPIC_CONTACT, /* `subject` is the contact's name */
    MESH_RADIO_BACKUP_TOPIC_COUNT,
};

enum mesh_radio_backup_change_kind {
    MESH_RADIO_BACKUP_CHANGED = 0,
    MESH_RADIO_BACKUP_ADDED,   /* only in the second */
    MESH_RADIO_BACKUP_REMOVED, /* only in the first */
};

enum mesh_radio_backup_value_kind {
    MESH_RADIO_BACKUP_VALUE_ABSENT = 0, /* not set, or not there at all */
    /* Set, and different, but not something a line of text can show: a key, a list. */
    MESH_RADIO_BACKUP_VALUE_OPAQUE,
    MESH_RADIO_BACKUP_VALUE_BOOL,
    MESH_RADIO_BACKUP_VALUE_INT,
    MESH_RADIO_BACKUP_VALUE_UINT,
    /* `number` over 10^`decimals`: a coordinate kept in millionths of a degree. */
    MESH_RADIO_BACKUP_VALUE_DECIMAL,
    MESH_RADIO_BACKUP_VALUE_TEXT,
};

struct mesh_radio_backup_value {
    uint8_t kind; /* enum mesh_radio_backup_value_kind */
    uint8_t decimals;
    int64_t number;
    char text[MESH_RADIO_BACKUP_VALUE_TEXT_MAX];
};

struct mesh_radio_backup_change {
    uint8_t kind;  /* enum mesh_radio_backup_change_kind */
    uint8_t topic; /* enum mesh_radio_backup_topic */
    uint16_t index;
    /* The protocol's number for the field, 0 for the topic as a whole. A field inside a nested
       message is its parent's number times 100 plus its own. */
    uint16_t field;
    char subject[MESH_RADIO_BACKUP_SUBJECT];
    struct mesh_radio_backup_value before;
    struct mesh_radio_backup_value after;
};

struct mesh_radio_backup_diff {
    uint8_t protocol; /* enum mesh_radio_backup_protocol, both backups' */
    size_t count;     /* in `changes` */
    size_t total;     /* found, kept or not */
    struct mesh_radio_backup_change changes[MESH_RADIO_BACKUP_DIFF_MAX];
};

void mesh_radio_backup_diff_reset(struct mesh_radio_backup_diff *diff, uint8_t protocol);

/* Counts one change and returns where to write it, or NULL when the list is full. Zeroed. */
struct mesh_radio_backup_change *mesh_radio_backup_diff_add(struct mesh_radio_backup_diff *diff,
                                                            uint8_t kind, uint8_t topic,
                                                            uint16_t index, uint16_t field);

/* The value setters a protocol's diff fills `before` and `after` with. */
void mesh_radio_backup_value_bool(struct mesh_radio_backup_value *value, bool b);
void mesh_radio_backup_value_int(struct mesh_radio_backup_value *value, int64_t number);
void mesh_radio_backup_value_uint(struct mesh_radio_backup_value *value, uint64_t number);
void mesh_radio_backup_value_decimal(struct mesh_radio_backup_value *value, int64_t number,
                                     uint8_t decimals);
/* At most `len` bytes of `text`, cut at a whole character. */
void mesh_radio_backup_value_text(struct mesh_radio_backup_value *value, const char *text,
                                  size_t len);
void mesh_radio_backup_value_opaque(struct mesh_radio_backup_value *value);

#ifdef __cplusplus
}
#endif
