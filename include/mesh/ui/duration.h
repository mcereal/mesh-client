#ifndef MESH_UI_DURATION_H
#define MESH_UI_DURATION_H

/*
 * How long ago, and how long for - as words, once.
 *
 * Two of the smallest functions in the client and the two most often written twice. Both are a
 * ladder of unit thresholds over the catalog's `TIME_` ids, and a ladder is exactly the kind of
 * thing that is easier to re-type than to go and find: the node detail and the Waypoints list
 * each carried a byte-identical copy of the age one, each with a comment saying it had to agree
 * with the other. A comment is not a mechanism, and two ladders drifting apart is a client that
 * calls the same gap four minutes on one screen and three on the next.
 *
 * They live here rather than in src/utils/ because what they answer with is a *string id*: the
 * thresholds are arithmetic, but "%um ago" is the i18n layer's to answer for and a locale may
 * put the unit in front of the number. That makes this a UI file that happens to hold no pixels,
 * which is the same seam layout.c sits on.
 */

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * "4m ago", "3h ago", "2d ago" - a stamp against the clock that is reading it.
 *
 * Both are in whole seconds and both are the *same* clock: a wall-clock stamp read against a
 * monotonic now is a number, not an age. An unset or future stamp reads as the short unknown
 * rather than as a wrapped enormous age, because a Brick with no RTC boots into the epoch and
 * every stamp on it is briefly in the future.
 */
void mesh_ui_format_age(uint32_t stamp, uint32_t now, char *out, size_t out_len);

/*
 * "2d 4h", "3h 20m", "45m" - a length of time, in seconds, that is not measured from now.
 *
 * Two units at the top of the ladder and one at the bottom, which is where this differs from an
 * age: an age is a glance and rounds hard, where a duration is usually the thing being read -
 * an uptime, or the span a chart's x axis covers - and "3h" for anything from three hours to
 * four is a picture whose axis is a range rather than a number. Under an hour there is no second
 * unit worth having, so it stays one.
 */
void mesh_ui_format_duration(uint32_t seconds, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_DURATION_H */
