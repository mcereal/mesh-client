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

/*
 * Seconds from the wall clock, for the things that are *drawn* from it: a message's "18:47", a
 * node's "3m", the day separator that says "Yesterday". Deadlines do not come from here - they
 * come from the monotonic clock above, for the reason stated there.
 *
 * It is a seam rather than a plain time(NULL) because of what reads it. A screenshot rendered
 * from the same source at two different minutes is two different files, so the pictures the
 * README and the Pak Store carry could not be regenerated without churning; the capture harness
 * pins this and the frames come out identical on any host at any hour. Nothing on the device
 * pins it, so there the clock is the clock.
 *
 * Returns 0 if the clock read fails, which callers already treat as "no reading".
 */
uint32_t mesh_time_wall_s(void);

/*
 * Freeze mesh_time_wall_s() at `epoch`, or pass 0 to follow the real clock again. Devtools and
 * tests only - the client never calls it.
 */
void mesh_time_wall_set_fixed(uint32_t epoch);

#ifdef __cplusplus
}
#endif

#endif /* MESH_TIME_H */
