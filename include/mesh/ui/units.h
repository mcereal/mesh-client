#ifndef MESH_UI_UNITS_H
#define MESH_UI_UNITS_H

/*
 * One system of units, and every length the client words in it.
 *
 * DisplayConfig.units is the radio's own metric/imperial preference, and this client follows it
 * rather than keeping a second one: a reader who set their radio to miles is not asked to set the
 * Brick to miles as well. That makes the preference a single byte off the wire and this module the
 * only place that reads it - mesh_ui_units_imperial() is the one `== 1U`, so a screen asks a
 * question rather than repeating an enum's encoding.
 *
 * The formatters take the answer as a bool rather than reading a global, which is the same choice
 * the rest of the UI layer makes about the store: the caller already has the snapshot the byte came
 * from, and a length that depends on hidden state cannot be checked by a test that does not also
 * arrange that state.
 *
 * Each one pairs a metric wording with an imperial one and picks the *scale* the same way in both:
 * a whole small unit below the threshold where it stops being walkable, and one decimal of the
 * large unit above it. What that threshold is differs - a kilometre and a mile are not the same
 * distance - but the reading does not, which is what stops the same fact looking coarser to one
 * reader than the other.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether DisplayConfig.units means imperial. 0 is metric and 1 is imperial; anything else is a
   value this build does not know, and metric is the protobuf's own default. */
bool mesh_ui_units_imperial(uint8_t units);

/*
 * A distance between two places: "820 m", "1.2 km", "2690 ft", "0.8 mi".
 *
 * Under a kilometre or a mile the figure is whole - a metre of precision on a mesh position is
 * already more than the sender knows - and above it there is one decimal, because 1 km and 2 km
 * with nothing between them is a scale nobody can walk by. A negative distance, and NaN, read as
 * unknown rather than as a number.
 */
void mesh_ui_format_distance(double metres, bool imperial, char *out, size_t out_len);

/*
 * A height above sea level: "312 m", "1024 ft". Whole units in both systems and signed, because a
 * fix below sea level is a real place rather than a bad reading.
 *
 * Not mesh_ui_format_distance(): an altitude does not become kilometres at a thousand metres, and
 * a node on a mountain reading "3.1 km" would be a height that looks like a range.
 *
 * Takes any int32_t, including the ones no height can be. Nothing upstream range-checks a height
 * the way it does a coordinate, so this is the one formatter here whose argument is not bounded
 * by the branch that reads it - see the note in units.c on why the conversion is integer
 * arithmetic and why the feet are wider than the metres.
 */
void mesh_ui_format_altitude(int32_t metres, bool imperial, char *out, size_t out_len);

/*
 * A length a *setting* is expressed in, where the value on the wire is metres: "500 m", "2 km",
 * "1640 ft", "1.2 mi".
 *
 * The metric side keeps whole kilometres for a round thousand and whole metres otherwise, because
 * these are numbers somebody picked off a list rather than a measurement - "1 km" is the preset and
 * "1.0 km" is arithmetic done to it. The imperial side has no round values to preserve, so it reads
 * as mesh_ui_format_distance() does.
 */
void mesh_ui_format_length(uint32_t metres, bool imperial, char *out, size_t out_len);

#endif /* MESH_UI_UNITS_H */
