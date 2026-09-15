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

bool mesh_ui_units_imperial(uint8_t units) {
    return units == 1U;
}

void mesh_ui_format_distance(double metres, bool imperial, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (!(metres >= 0.0)) {
        /* Also the NaN case, which is why the test is written this way round. */
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT));
        return;
    }

    if (imperial) {
        if (metres < MESH_UI_METRES_PER_MILE) {
            mesh_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_FT,
                            (unsigned)(metres / MESH_UI_METRES_PER_FOOT + 0.5));
            return;
        }
        mesh_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_MI, metres / MESH_UI_METRES_PER_MILE);
        return;
    }
    if (metres < MESH_UI_DISTANCE_KM_FROM) {
        mesh_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_M, (unsigned)(metres + 0.5));
        return;
    }
    mesh_str_format(out, out_len, MESH_STR_VALUE_DISTANCE_KM, metres / MESH_UI_DISTANCE_KM_FROM);
}

void mesh_ui_format_altitude(int32_t metres, bool imperial, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (!imperial) {
        mesh_str_format(out, out_len, MESH_STR_VALUE_ALTITUDE_M, (int)metres);
        return;
    }
    /* Rounded away from zero on both sides, so a metre below sea level does not read as 0 ft. */
    const double feet = (double)metres / MESH_UI_METRES_PER_FOOT;
    mesh_str_format(out, out_len, MESH_STR_VALUE_ALTITUDE_FT,
                    (int)(feet >= 0.0 ? feet + 0.5 : feet - 0.5));
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
        mesh_str_format(out, out_len, MESH_STR_VALUE_KILOMETRES, (unsigned)(metres / 1000U));
        return;
    }
    mesh_str_format(out, out_len, MESH_STR_VALUE_METRES, (unsigned)metres);
}
