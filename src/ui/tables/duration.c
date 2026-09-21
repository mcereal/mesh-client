#include "mesh/ui/duration.h"

#include "inkwell/base/text.h"

#include "mesh/i18n/strings.h"

void mesh_ui_format_age(uint32_t stamp, uint32_t now, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (stamp == 0U || now == 0U || stamp > now) {
        inkwell_str_copy(out, out_len, inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT));
        return;
    }
    const uint32_t seconds = now - stamp;
    if (seconds < 60U) {
        inkcell_str_format(out, out_len, MESH_STR_TIME_AGO_SECONDS, seconds);
    } else if (seconds < 3600U) {
        inkcell_str_format(out, out_len, MESH_STR_TIME_AGO_MINUTES, seconds / 60U);
    } else if (seconds < 86400U) {
        inkcell_str_format(out, out_len, MESH_STR_TIME_AGO_HOURS, seconds / 3600U);
    } else {
        inkcell_str_format(out, out_len, MESH_STR_TIME_AGO_DAYS, seconds / 86400U);
    }
}

void mesh_ui_format_duration(uint32_t seconds, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (seconds >= 86400U) {
        inkcell_str_format(out, out_len, MESH_STR_TIME_DAYS_HOURS, seconds / 86400U,
                           (seconds % 86400U) / 3600U);
    } else if (seconds >= 3600U) {
        inkcell_str_format(out, out_len, MESH_STR_TIME_HOURS_MINUTES, seconds / 3600U,
                           (seconds % 3600U) / 60U);
    } else if (seconds >= 60U) {
        inkcell_str_format(out, out_len, INKCELL_STR_TIME_MINUTES_SHORT, seconds / 60U);
    } else {
        /* Under a minute is still a duration and still has to be one: a span drawn as "0m" says
           the picture covers nothing, where the readings on it are seconds apart. The uptime
           this replaced rounded to "0m" here, which was never reachable on a radio that had been
           up long enough to report an uptime at all. */
        inkcell_str_format(out, out_len, INKCELL_STR_TIME_SECONDS_SHORT, seconds);
    }
}
