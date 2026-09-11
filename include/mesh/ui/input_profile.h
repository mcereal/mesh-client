#ifndef MESH_UI_INPUT_PROFILE_H
#define MESH_UI_INPUT_PROFILE_H

/*
 * What the case looks like: which evdev code each printed button reports, and what is printed
 * on it.
 *
 * Those two facts used to be stated in two files. src/ui/input.c held the codes - the Brick's A
 * is BTN_EAST because its face buttons are laid out the way a Nintendo pad's are - and
 * src/ui/actions.c held the caps, a table saying the button called A is printed "A". Both are
 * one fact about one piece of plastic, and a second device is what makes the split expensive: a
 * pad following the Xbox convention reports BTN_SOUTH for the button printed A, so a port that
 * corrected the codes and not the caps would leave the action bar naming a key that does
 * something else - silently, because the binding still works, just not the one the bar promised.
 *
 * That is the same two-opinions-about-one-fact problem the app bar's back arrow and the Status
 * verb list each solved by reading one table, and this is that table. A device is a row in
 * k_profiles[] and nothing else.
 *
 * A profile states only what the *case* decides. Everything a convention decides - the arrow and
 * Enter keys of a USB keyboard, the hat axes a d-pad reports, the shoulders, START and SELECT -
 * is the same on every device that speaks evdev, so it stays in src/ui/input.c and is consulted
 * after the profile. A profile may still bind one of those codes itself and win, which is what
 * keeps a future device with an odd pad a table row rather than a patch to the conventions.
 *
 * MESHCLIENT_INPUT_PROFILE names one, the same way MESHCLIENT_THEME names a look; with nothing
 * set the Brick's is used, because that is the device this pak ships for. There is deliberately
 * no auto-detection: a pad's evdev name is not a promise about its silkscreen, and guessing
 * wrong swaps confirm and back - see the note on mesh_ui_input_profile_from_env().
 */

#include "mesh/ui/actions.h"
#include "mesh/ui/nav.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One evdev code and the logical key the button reporting it is printed with. */
struct mesh_ui_input_binding {
    uint16_t code;
    enum mesh_ui_key key;
};

/*
 * One device's pad.
 *
 * `caps` is indexed by enum mesh_ui_button and is stated in full, including the entries no
 * profile is ever likely to change - a profile that filled in only its differences would be a
 * device described by what it is not. MESH_UI_BUTTON_QUIT is the one entry left NULL on
 * purpose: the key that leaves the pak is whatever MESHCLIENT_QUIT_KEYS says, so the module
 * that parsed that variable is the one that can name it. See mesh_ui_button_cap().
 *
 * Two profiles may share a cap table - the Brick and an Xbox-convention pad print the same four
 * letters and differ only in where they put them - and that is the pairing this struct exists
 * to keep visible: the codes and the words sit in one row, so a device whose buttons are
 * printed something else cannot pick up the wrong half.
 */
struct mesh_ui_input_profile {
    const char *name; /* what MESHCLIENT_INPUT_PROFILE matches, lower case */
    const struct mesh_ui_input_binding *bindings;
    size_t binding_count;
    const char *const *caps; /* MESH_UI_BUTTON_COUNT entries */
};

/* The registry, in the order it is written. */
size_t mesh_ui_input_profile_count(void);
const struct mesh_ui_input_profile *mesh_ui_input_profile_at(size_t index);
/* NULL when `name` is NULL, empty or unknown. Case-insensitive. */
const struct mesh_ui_input_profile *mesh_ui_input_profile_by_name(const char *name);
/* The Brick's, which is what this pak ships for. Never NULL. */
const struct mesh_ui_input_profile *mesh_ui_input_profile_default(void);

/*
 * The profile MESHCLIENT_INPUT_PROFILE names, or the default. Never NULL.
 *
 * Read once and cached, exactly as the quit keys are: which pad this is cannot change while the
 * client runs, and a cap re-resolved per frame would be a lookup per keycap drawn.
 *
 * An unknown name logs and falls back rather than failing the client's startup. A profile is a
 * correction to the *labels* - the client is perfectly usable with the wrong one, just
 * confusing - so refusing to start over a typo would be the worse failure.
 */
const struct mesh_ui_input_profile *mesh_ui_input_profile_from_env(void);

/* Exposed so tests can re-read MESHCLIENT_INPUT_PROFILE after changing it. */
void mesh_ui_input_profile_reload(void);

/*
 * The key this profile binds `code` to, or MESH_UI_KEY_NONE when it binds nothing to it -
 * which is the ordinary answer, since a profile holds four face buttons and the conventions
 * hold the rest.
 */
enum mesh_ui_key mesh_ui_input_profile_key(const struct mesh_ui_input_profile *profile,
                                           uint16_t code);

/*
 * The cap printed beside `button`, or "" for one this profile does not name (the quit key, and
 * anything outside the enum). Never NULL, for the same reason mesh_ui_button_cap() is not: a
 * bar that drew a NULL would be a crash in the one place a wrong answer is merely cosmetic.
 */
const char *mesh_ui_input_profile_cap(const struct mesh_ui_input_profile *profile,
                                      enum mesh_ui_button button);

/*
 * Whether a profile is complete: every face button bound exactly once, no code bound twice, and
 * a cap for every button but the quit key.
 *
 * The theme's contrast contract one layer over: a table that anyone may add a row to needs
 * something that holds the row to the shape, or the first device added after this comment is
 * written arrives with three face buttons and a missing cap. `reason` is filled on failure.
 */
bool mesh_ui_input_profile_validate(const struct mesh_ui_input_profile *profile, char *reason,
                                    size_t reason_len);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_INPUT_PROFILE_H */
