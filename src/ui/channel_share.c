#define _POSIX_C_SOURCE 200809L

/* See mesh/ui/channel_share.h. Reads a channel link and answers in catalog text. */

#include "mesh/ui/channel_share.h"

#include "mesh/i18n/strings.h"
#include "mesh/proto/channel_url.h"
#include "mesh/ui/store_settings.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * The store's buffer has to hold the longest link the wire format can make.
 *
 * store_settings.h restates the bound rather than including this header, for the reason it
 * gives there; this is the assertion that keeps the restatement honest. A protobuf bump that
 * widens ChannelSet fails the build here rather than silently cutting a share in half.
 */
_Static_assert(MESH_UI_CHANNEL_URL_MAX >= MESH_CHANNEL_URL_MAX,
               "the store's share_url is too small for the longest channel link");

/* The name to put in front of a person. An empty one is not a channel without a name: upstream
   reads it as the preset's own default channel, and every app shows it that way. */
static const char *channel_name(const meshtastic_ChannelSettings *settings) {
    return settings->name[0] != '\0' ? settings->name : mesh_str(MESH_STR_CHANNELS_DEFAULT_NAME);
}

bool mesh_ui_channel_share_summary(const char *url, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';
    meshtastic_ChannelSet set;
    if (url == NULL || url[0] == '\0' || !mesh_channel_url_decode(url, &set, NULL)) {
        return false;
    }
    mesh_str_format_plural(out, out_len, MESH_STR_SHARE_SUMMARY_ONE, (uint32_t)set.settings_count,
                           (unsigned)set.settings_count);
    return true;
}

bool mesh_ui_channel_link_valid(const char *text) {
    meshtastic_ChannelSet set;
    return text != NULL && mesh_channel_url_decode(text, &set, NULL);
}

bool mesh_ui_channel_import_sheet(const char *text, char *headline, size_t headline_len, char *body,
                                  size_t body_len) {
    if (headline != NULL && headline_len > 0U) {
        headline[0] = '\0';
    }
    if (body != NULL && body_len > 0U) {
        body[0] = '\0';
    }
    meshtastic_ChannelSet set;
    if (text == NULL || !mesh_channel_url_decode(text, &set, NULL)) {
        return false;
    }
    if (headline != NULL) {
        mesh_str_format(headline, headline_len, MESH_STR_CONFIRM_TITLE_IMPORT,
                        channel_name(&set.settings[0]));
    }
    if (body != NULL) {
        mesh_str_format_plural(body, body_len, MESH_STR_CONFIRM_TEXT_IMPORT_ONE,
                               (uint32_t)set.settings_count, (unsigned)set.settings_count);
    }
    return true;
}
