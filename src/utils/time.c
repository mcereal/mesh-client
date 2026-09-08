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

/*
 * Zero means "follow the real clock", which is also what a caller that never pins it gets. A
 * plain file-scope value rather than anything cleverer: this is read on the drawing path, and
 * the only writer is a harness that runs before the first frame.
 */
static uint32_t g_wall_fixed = 0U;

uint32_t mesh_time_wall_s(void) {
    if (g_wall_fixed != 0U) {
        return g_wall_fixed;
    }
    const time_t now = time(NULL);
    if (now <= 0) {
        return 0U;
    }
    return (uint32_t)now;
}

uint32_t mesh_time_wall_credible_s(void) {
    const uint32_t now = mesh_time_wall_s();
    return now > MESH_TIME_CLOCK_MIN_EPOCH ? now : 0U;
}

void mesh_time_wall_set_fixed(uint32_t epoch) { g_wall_fixed = epoch; }
