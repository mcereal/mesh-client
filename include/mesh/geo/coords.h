#ifndef MESH_GEO_COORDS_H
#define MESH_GEO_COORDS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Coordinates, and the one question every ingress has to ask about them.
 *
 * Meshtastic carries latitude and longitude as fixed-point 1e-7 degrees in an sfixed32, which
 * is four times wider than the range a coordinate can occupy: the wire can say 2000000000, and
 * that is 200 degrees of latitude. Nothing upstream promises otherwise, so a coordinate is
 * checked where it enters rather than trusted because of where it came from.
 *
 * This header deliberately knows nothing about protobufs, the UI store or the framebuffer -
 * it is the first piece of the `geo` module docs/maps-roadmap.md proposes, and the map layer
 * that arrives later reuses the same test rather than writing a second one.
 */

/* 90 and 180 degrees at 1e-7, the extremes of each axis. Both ends are inclusive: the poles
   and the antimeridian are real places. */
#define MESH_GEO_LATITUDE_I_MAX 900000000
#define MESH_GEO_LONGITUDE_I_MAX 1800000000

/*
 * Whether a fixed-point pair names a point on Earth.
 *
 * Null Island is one of them. `(0, 0)` is where a node with a half-initialised GPS most often
 * claims to be, but it is also a real point in the Gulf of Guinea, and a validator that
 * rejected it would be a guess about the sender's firmware wearing a range check's clothes.
 * A packet carrying no coordinates at all is a different question, answered by the has_ bits
 * at the call site, and the answer there is to keep the previous fix rather than to erase it.
 */
bool mesh_geo_coords_valid(int32_t latitude_i, int32_t longitude_i);

#endif /* MESH_GEO_COORDS_H */
