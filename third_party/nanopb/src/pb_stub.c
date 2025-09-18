#include "pb_common.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <stdio.h>

bool pb_encode(pb_ostream_t *stream, const void *fields, const void *src_struct) {
    (void)stream;
    (void)fields;
    (void)src_struct;
    fprintf(stderr, "pb_encode stub called\n");
    return false;
}

bool pb_decode(pb_istream_t *stream, const void *fields, void *dest_struct) {
    (void)stream;
    (void)fields;
    (void)dest_struct;
    fprintf(stderr, "pb_decode stub called\n");
    return false;
}
