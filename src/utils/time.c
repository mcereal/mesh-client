#define _POSIX_C_SOURCE 200809L

#include "mesh/utils/time.h"

#include <time.h>

uint64_t mesh_time_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}
