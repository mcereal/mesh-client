#pragma once

#include "pb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*pb_callback_t)(pb_istream_t *istream, pb_ostream_t *ostream, void *arg);

#ifdef __cplusplus
}
#endif
