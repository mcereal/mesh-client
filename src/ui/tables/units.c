#define _POSIX_C_SOURCE 200809L

/*
 * Lengths, in whichever system the radio's display is set to.
 *
 * Nothing here decides *which* system - the caller has the settings snapshot and asks
 * mesh_ui_units_imperial() about it - and nothing here spells a unit out: every wording is a
 * catalog id, so "1.2 km" and "0.8 mi" are a locale's business exactly as the rest of the UI's
 * text is. See include/mesh/ui/units.h for why each formatter picks the scale it does.
 */

#include "mesh/ui/units.h"

#include "mesh/i18n/strings.h"

#include <stdio.h>

/* Where a distance stops being a walk and starts being a journey, in each system of units. */
#define MESH_UI_DISTANCE_KM_FROM 1000.0
#define MESH_UI_METRES_PER_FOOT 0.3048
#define MESH_UI_METRES_PER_MILE 1609.344

bool mesh_ui_units_imperial(uint8_t units) { return units == 1U; }

void mesh_ui_format_distance(double metres, bool imperial, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (!(metres >= 0.0)) {
        /* Also the NaN case, which is why the test is written this way round. */
        snprintf(out, out_len, "%s", inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT));
        return;
    }

    if (imperial) {
        if (metres < MESH_UI_METRES_PER_MILE) {
            inkcell_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_FT,
                               (unsigned)(metres / MESH_UI_METRES_PER_FOOT + 0.5));
            return;
        }
        inkcell_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_MI,
                           metres / MESH_UI_METRES_PER_MILE);
        return;
    }
    if (metres < MESH_UI_DISTANCE_KM_FROM) {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_M, (unsigned)(metres + 0.5));
        return;
    }
    inkcell_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_KM, metres / MESH_UI_DISTANCE_KM_FROM);
}

void mesh_ui_format_altitude(int32_t metres, bool imperial, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (!imperial) {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_ALTITUDE_M, (int)metres);
        return;
    }
    /*
     * Integer arithmetic rather than a divide and a cast, and a wider type than the metres came
     * in, because this is the one formatter here with no branch bounding its argument.
     *
     * `altitude` is whatever int32_t the sender put in the packet - the session range-checks a
     * coordinate and copies the height through unchanged, which is the right way round: a height
     * has no range to check it against the way a latitude does. The range that does exist is the
     * int32 itself, and three metres of it are more than four in feet: INT32_MAX metres is about
     * 7.05e9 feet, so `(int)(metres / 0.3048)` is a float-to-int conversion out of range, which
     * is undefined - a garbage reading in a release build and an abort under the sanitizer CI
     * runs. A long long holds every value the conversion can produce, and the metric row prints
     * the same packet's number unchanged, so both systems are as faithful as each other.
     *
     * A foot is exactly 3048/10000 of a metre, so the division is exact rather than nearly so,
     * and the half added before it rounds away from zero on both sides - a metre below sea level
     * reads as -3 ft rather than as 0.
     */
    const long long scaled = (long long)metres * 10000LL;
    const long long half = metres >= 0 ? 1524LL : -1524LL;
    inkcell_str_format(out, out_len, MESH_STR_VALUE_ALTITUDE_FT, (scaled + half) / 3048LL);
}

void mesh_ui_format_length(uint32_t metres, bool imperial, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (imperial) {
        mesh_ui_format_distance((double)metres, true, out, out_len);
        return;
    }
    if (metres >= 1000U && metres % 1000U == 0U) {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_KILOMETRES, (unsigned)(metres / 1000U));
        return;
    }
    inkcell_str_format(out, out_len, MESH_STR_VALUE_METRES, (unsigned)metres);
}
