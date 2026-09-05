#ifndef MESH_TIME_H
#define MESH_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Milliseconds from CLOCK_MONOTONIC.
 *
 * Every subsystem that schedules anything - the session's handshake timeouts, both transports'
 * reconnect backoff, the app's auto-connect and the store's toasts - needs the same number, and
 * each had grown its own byte-identical copy of this. It is monotonic rather than wall-clock on
 * purpose: the Brick has no RTC battery, so its wall clock jumps once NTP lands and every
 * deadline computed from it would fire early or never.
 *
 * Returns 0 if the clock read fails, which no caller can distinguish from "the machine just
 * booted" - that is deliberate, because a timeout that fires immediately is a retry and a
 * timeout that never fires is a hang.
 */
uint64_t mesh_time_monotonic_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_TIME_H */
