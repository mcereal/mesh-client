#include "mesh/ui/reactions.h"

#include "mesh/utils/array.h"

/*
 * The eight, in the order the picker walks them: agreement first, because it is what almost
 * every tapback is, and the two that ask something of the sender last.
 *
 * Each glyph is written as its UTF-8 bytes rather than pasted in, so a terminal, an editor or a
 * patch tool that mangles non-ASCII cannot quietly change what this client puts on the air.
 * They are the codepoints named in the comments and nothing else - no variation selectors, and
 * nothing that composes: fb_thread_reactions() counts a reaction by its first cell, so a glyph
 * that is two codepoints would be counted as a different one from the same glyph sent by a
 * phone that spelled it the other way.
 */
static const struct {
    const char *emoji;
    enum mesh_str_id label;
} k_reactions[] = {
    {"\xF0\x9F\x91\x8D", MESH_STR_REACTION_THUMBS_UP},   /* U+1F44D thumbs up */
    {"\xF0\x9F\x91\x8E", MESH_STR_REACTION_THUMBS_DOWN}, /* U+1F44E thumbs down */
    {"\xE2\x9D\xA4", MESH_STR_REACTION_HEART},           /* U+2764 heavy black heart */
    {"\xF0\x9F\x98\x82", MESH_STR_REACTION_LAUGH},       /* U+1F602 face with tears of joy */
    {"\xF0\x9F\x98\xAE", MESH_STR_REACTION_SURPRISE},    /* U+1F62E face with open mouth */
    {"\xF0\x9F\x98\xA2", MESH_STR_REACTION_SAD},         /* U+1F622 crying face */
    {"\xE2\x9D\x93", MESH_STR_REACTION_QUESTION},        /* U+2753 black question mark ornament */
    {"\xE2\x80\xBC", MESH_STR_REACTION_IMPORTANT},       /* U+203C double exclamation mark */
};

size_t mesh_ui_reaction_count(void) { return MESH_ARRAY_LEN(k_reactions); }

const char *mesh_ui_reaction_emoji(size_t index) {
    if (index >= MESH_ARRAY_LEN(k_reactions)) {
        return "";
    }
    return k_reactions[index].emoji;
}

enum mesh_str_id mesh_ui_reaction_label(size_t index) {
    if (index >= MESH_ARRAY_LEN(k_reactions)) {
        return MESH_STR_NONE;
    }
    return k_reactions[index].label;
}
