#define _POSIX_C_SOURCE 200809L

#include "scene.h"

#include "inkcell/ui/fb_capture.h"
#include "inkcell/ui/fb_draw.h"
#include "inkcell/ui/theme.h"
#include "inkwell/base/time.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define UICAP_SCENE_DEFAULT_FRAME_MS 33U
#define UICAP_SCENE_DEFAULT_SETTLE_FRAMES 40U
#define UICAP_SCENE_DEFAULT_START_MS 1000U

/* GIF carries a delay in centiseconds in a 16-bit field, so 655350 ms is the ceiling anything
   downstream can express. */
#define UICAP_SCENE_DELAY_CEILING_MS 600000U

/* A typo guard, not a claim about what is sensible: a missed digit in a hold is ten minutes. */
#define UICAP_SCENE_NUMBER_MAX 600000L

int uicap_scene_fail(struct uicap_scene *scene, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(scene->error, sizeof scene->error, format, args);
    va_end(args);
    return -EINVAL;
}

/* The verbs the runner answers itself, before it looks in the application's table. */
static const char *const scene_generic_verbs[] = {
    "scene", "scale", "delay", "clock", "theme", "pointer", "key", "frame", "hold",
};

int uicap_scene_init(struct uicap_scene *scene, const struct uicap_scene_host *host,
                     const struct uicap_scene_sink *sink, const struct uicap_scene_config *config) {
    memset(scene, 0, sizeof *scene);
    if (host == NULL || host->capture == NULL || host->snapshot == NULL || host->drain == NULL ||
        host->press == NULL) {
        return uicap_scene_fail(scene, "the host needs a capture, a snapshot, drain and press");
    }
    for (size_t i = 0; i < host->verb_count; ++i) {
        if (host->verbs[i].name == NULL || host->verbs[i].run == NULL) {
            return uicap_scene_fail(scene, "verb %zu has no name or no run", i);
        }
        for (size_t j = 0; j < sizeof scene_generic_verbs / sizeof scene_generic_verbs[0]; ++j) {
            if (strcmp(host->verbs[i].name, scene_generic_verbs[j]) == 0) {
                return uicap_scene_fail(scene, "'%s' is the runner's own verb",
                                        host->verbs[i].name);
            }
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(host->verbs[i].name, host->verbs[j].name) == 0) {
                return uicap_scene_fail(scene, "two verbs are called '%s'", host->verbs[i].name);
            }
        }
    }
    scene->host = *host;
    if (sink != NULL) {
        scene->sink = *sink;
    }
    if (config != NULL) {
        scene->config = *config;
    }
    if (scene->config.frame_ms == 0U) {
        scene->config.frame_ms = UICAP_SCENE_DEFAULT_FRAME_MS;
    }
    if (scene->config.settle_frames == 0U) {
        scene->config.settle_frames = UICAP_SCENE_DEFAULT_SETTLE_FRAMES;
    }
    if (scene->config.start_ms == 0U) {
        scene->config.start_ms = UICAP_SCENE_DEFAULT_START_MS;
    }
    if (scene->config.theme != NULL) {
        snprintf(scene->theme, sizeof scene->theme, "%s", scene->config.theme);
    }
    if (host->seed_count > 0U) {
        snprintf(scene->seed, sizeof scene->seed, "%s", host->seeds[0].name);
    }
    /* A copy was taken, and the pointer would outlive whatever it pointed at. */
    scene->config.theme = NULL;
    scene->now_ms = scene->config.start_ms;
    return 0;
}

const char *uicap_scene_error(const struct uicap_scene *scene) { return scene->error; }
unsigned uicap_scene_error_line(const struct uicap_scene *scene) { return scene->line; }
uint64_t uicap_scene_now(const struct uicap_scene *scene) { return scene->now_ms; }
struct inkcell_capture *uicap_scene_capture(const struct uicap_scene *scene) {
    return scene->host.capture;
}
bool uicap_scene_started(const struct uicap_scene *scene) { return scene->started; }

/* ---- the words ----------------------------------------------------------------------------- */

char *uicap_scene_word(char **rest) {
    char *cursor = *rest;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor == '\0') {
        *rest = cursor;
        return NULL;
    }
    char *start = cursor;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
        cursor++;
    }
    if (*cursor != '\0') {
        *cursor++ = '\0';
    }
    *rest = cursor;
    return start;
}

char *uicap_scene_tail(char *rest) {
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    return rest;
}

int uicap_scene_number(struct uicap_scene *scene, const char *text, const char *what,
                       unsigned *out) {
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || text[0] == '-' ||
        value > (unsigned long)UICAP_SCENE_NUMBER_MAX) {
        return uicap_scene_fail(scene, "%s: '%s' is not a number I can use", what, text);
    }
    *out = (unsigned)value;
    return 0;
}

/*
 * The same, for a reading that lives below zero.
 *
 * Its own parser rather than a sign bolted onto the unsigned one, because the bound is the whole
 * point of that function - it is a typo guard - and a guard written as "or negative, sometimes"
 * stops guarding anything.
 */
int uicap_scene_signed(struct uicap_scene *scene, const char *text, const char *what, int *out) {
    char *end = NULL;
    const long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < -UICAP_SCENE_NUMBER_MAX ||
        value > UICAP_SCENE_NUMBER_MAX) {
        return uicap_scene_fail(scene, "%s: '%s' is not a number I can use", what, text);
    }
    *out = (int)value;
    return 0;
}

/* ---- frames -------------------------------------------------------------------------------- */

/*
 * Steps the clock every frame is drawn against.
 *
 * The scene's own clock and the renderer's are the same clock: a `hold` is the frame sitting on
 * screen for that long, so a knob that was mid-slide when it started has moved by the time the
 * next line runs. Anything else would film the panel with a stopped watch.
 */
static void scene_advance(struct uicap_scene *scene, unsigned ms) {
    scene->now_ms += ms;
    inkcell_capture_advance(scene->host.capture, ms);
    /* The housekeeping a loop does on every turn. Without it the scene's clock ran but nothing
       timed out, so a notice stayed up for the rest of the script, and one sliding *away* - the
       half of that transition a still cannot show - was not filmable at all. */
    if (scene->host.tick != NULL) {
        scene->host.tick(scene->host.userdata, scene->now_ms);
    }
}

/* Hands the sink the delay of the frame before this one, which nothing can lengthen any more. */
static int scene_flush_delay(struct uicap_scene *scene) {
    if (scene->frames == 0U || scene->sink.delay == NULL) {
        return 0;
    }
    const int status =
        scene->sink.delay(scene->sink.userdata, scene->frames - 1U, scene->pending_delay_ms);
    if (status < 0) {
        return uicap_scene_fail(scene, "cannot record frame %u's delay: %s", scene->frames,
                                strerror(-status));
    }
    return 0;
}

static int scene_emit_delay(struct uicap_scene *scene, unsigned delay_ms) {
    struct uicap_scene_host *host = &scene->host;
    if (!host->drain(host->userdata, host->snapshot)) {
        /* Nothing changed - a press the screen ignores, say. Draw it anyway: a clip that silently
           drops the frames where nothing happened is a clip that lies about what the button
           did. */
        if (host->refresh != NULL) {
            host->refresh(host->userdata);
        }
        (void)host->drain(host->userdata, host->snapshot);
    }
    inkcell_capture_render(host->capture, host->snapshot);

    int status = scene_flush_delay(scene);
    if (status < 0) {
        return status;
    }
    if (scene->sink.frame != NULL) {
        status = scene->sink.frame(scene->sink.userdata, scene->frames, host->capture);
        if (status < 0) {
            return uicap_scene_fail(scene, "cannot write frame %u: %s", scene->frames + 1U,
                                    strerror(-status));
        }
    }
    scene->frames++;

    /*
     * A frame that leaves something mid-transition carries the animation's interval, whatever
     * the scene asked for. The scene's delay is for a frame somebody is meant to read, and the
     * frame a press emits is not one: the knob has not moved yet, so holding it for the scene's
     * delay froze the old state for a third of a second before every slide - a pause the device
     * does not have and the clip should not invent. The frame the transition lands on is not
     * animating any more, so it keeps the scene's delay and a `hold` after it still works.
     */
    if (inkcell_capture_animating(host->capture) && delay_ms > scene->config.frame_ms) {
        delay_ms = scene->config.frame_ms;
    }
    scene->pending_delay_ms = delay_ms;
    return 0;
}

int uicap_scene_emit(struct uicap_scene *scene) {
    return scene_emit_delay(scene, scene->config.delay_ms);
}

/*
 * Plays out whatever the last frame left moving.
 *
 * A press that flips a switch does not finish on the frame that handled it - the knob is a few
 * pixels into a slide. The renderer says so, so the runner keeps stepping the clock and drawing
 * until it stops, exactly as a loop's frame timer does on a device. That is what makes an
 * animation reviewable without a single script having to know an animation exists.
 *
 * The cap was a guard against a widget that never settles - a bug, but not one that should hang
 * a capture - and it is also the length of one legitimate case: an *indeterminate* meter loops
 * for as long as it is on screen and has no landing frame to reach, so it films until the cap
 * and stops. A scene that wants more asks for it with another `frame`.
 */
int uicap_scene_settle(struct uicap_scene *scene) {
    for (unsigned i = 0U; i < scene->config.settle_frames; ++i) {
        if (!inkcell_capture_animating(scene->host.capture)) {
            return 0;
        }
        scene_advance(scene, scene->config.frame_ms);
        const int status = uicap_scene_emit(scene);
        if (status < 0) {
            return status;
        }
    }
    return 0;
}

int uicap_scene_press(struct uicap_scene *scene, enum inkcell_key key) {
    scene->host.press(scene->host.userdata, key, inkcell_capture_page_rows(scene->host.capture));
    const int status = uicap_scene_emit(scene);
    return status < 0 ? status : uicap_scene_settle(scene);
}

/* Lengthens the frame just emitted rather than emitting a duplicate: a still moment in a clip is
   one frame that lingers, not twenty identical ones. */
static int scene_hold(struct uicap_scene *scene, unsigned extra_ms) {
    if (scene->frames == 0U) {
        return uicap_scene_fail(scene, "hold: nothing has been captured yet");
    }
    const unsigned held = scene->pending_delay_ms + extra_ms;
    scene->pending_delay_ms =
        held > UICAP_SCENE_DELAY_CEILING_MS ? UICAP_SCENE_DELAY_CEILING_MS : held;
    scene_advance(scene, extra_ms);
    return 0;
}

/* ---- the look ------------------------------------------------------------------------------ */

/*
 * Picks the theme by name, and re-applies the scale because a theme carries one of its own.
 * A scale of 0 means "whatever this theme asks for", which is what makes `theme light` alone do
 * the right thing and `scale 5` still win when a script says both.
 */
static int scene_apply_theme(struct uicap_scene *scene, const char *name) {
    const struct inkcell_theme *theme = inkcell_theme_by_id(name);
    if (theme == NULL) {
        char known[128] = "";
        size_t used = 0U;
        for (size_t i = 0; i < inkcell_theme_count() && used < sizeof known; ++i) {
            const int wrote =
                snprintf(known + used, sizeof known - used, " %s", inkcell_theme_at(i)->id);
            used += wrote > 0 ? (size_t)wrote : 0U;
        }
        return uicap_scene_fail(scene, "no theme called '%s'. Try:%s", name, known);
    }
    inkcell_capture_set_theme(scene->host.capture, theme);
    inkcell_capture_set_scale(scene->host.capture, scene->config.scale);
    if (scene->host.themed != NULL) {
        scene->host.themed(scene->host.userdata);
    }
    return 0;
}

static int scene_start(struct uicap_scene *scene) {
    if (scene->started) {
        return 0;
    }
    scene->started = true;
    if (scene->theme[0] != '\0') {
        const int status = scene_apply_theme(scene, scene->theme);
        if (status < 0) {
            return status;
        }
    }
    inkcell_capture_set_scale(scene->host.capture, scene->config.scale);
    if (scene->host.seed_count > 0U) {
        const struct uicap_scene_seed *seed = NULL;
        for (size_t i = 0; i < scene->host.seed_count; ++i) {
            if (strcmp(scene->host.seeds[i].name, scene->seed) == 0) {
                seed = &scene->host.seeds[i];
            }
        }
        if (seed == NULL) {
            return uicap_scene_fail(scene, "scene: no seed called '%s'", scene->seed);
        }
        const int status = seed->seed(scene, scene->host.userdata);
        if (status < 0) {
            return status;
        }
    }
    /* After the seed, which is free to publish what the application says about itself. */
    if (scene->host.themed != NULL) {
        scene->host.themed(scene->host.userdata);
    }
    if (scene->host.refresh != NULL) {
        scene->host.refresh(scene->host.userdata);
    }
    return uicap_scene_emit(scene);
}

/* ---- the generic verbs --------------------------------------------------------------------- */

static int scene_setup_value(struct uicap_scene *scene, const char *command, char **rest,
                             char **out) {
    if (scene->started) {
        return uicap_scene_fail(scene, "'%s' has to come before the first frame", command);
    }
    *out = uicap_scene_word(rest);
    if (*out == NULL) {
        return uicap_scene_fail(scene, "'%s' needs a value", command);
    }
    return 0;
}

/*
 * A fixed wall clock, as local time - "clock 2026-01-12 19:12".
 *
 * An application seeds its records against this and its renderer draws "18:47", "3m" and
 * "Yesterday" from the same value, so a scene renders the same frames on any host at any hour.
 * That is what a checked-in picture needs: without it, re-rendering an hour later rewrote every
 * pixel of a clock, and doing it either side of midnight moved the day separators.
 *
 * Local rather than UTC because it is read back through localtime_r: a time written here is the
 * time on the panel, whatever zone the machine rendering it is in.
 */
static int scene_clock(struct uicap_scene *scene, char *rest) {
    if (scene->started) {
        return uicap_scene_fail(scene, "'clock' has to come before the first frame");
    }
    const char *when = uicap_scene_tail(rest);
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    /* sscanf rather than strptime: the format is fixed, and strptime is behind a feature macro
       this file would otherwise have no reason to raise. */
    if (sscanf(when, "%4d-%2d-%2d %2d:%2d", &year, &month, &day, &hour, &minute) != 5) {
        return uicap_scene_fail(scene, "'clock' needs a local time as YYYY-MM-DD HH:MM");
    }
    struct tm parts;
    memset(&parts, 0, sizeof parts);
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_isdst = -1; /* let mktime work out the offset in force on that date */
    const time_t pinned = mktime(&parts);
    if (pinned <= 0) {
        return uicap_scene_fail(scene, "'clock' cannot represent that time here");
    }
    inkwell_time_wall_set_fixed((uint32_t)pinned);
    return 0;
}

/* Returns 1 when `command` was not one of these. */
static int scene_run_generic(struct uicap_scene *scene, const char *command, char *rest) {
    char *value = NULL;
    int status = 0;

    if (strcmp(command, "scene") == 0) {
        if ((status = scene_setup_value(scene, command, &rest, &value)) < 0) {
            return status;
        }
        snprintf(scene->seed, sizeof scene->seed, "%s", value);
        return 0;
    }
    if (strcmp(command, "scale") == 0) {
        unsigned scale = 0U;
        if ((status = scene_setup_value(scene, command, &rest, &value)) < 0 ||
            (status = uicap_scene_number(scene, value, "scale", &scale)) < 0) {
            return status;
        }
        /* In whole steps, as a script states it. A scale counts quarters of one inside - the
           conversion belongs where the outside world states a number. */
        scene->config.scale = INKCELL_SCALE((int)scale);
        return 0;
    }
    if (strcmp(command, "delay") == 0) {
        if ((status = scene_setup_value(scene, command, &rest, &value)) < 0) {
            return status;
        }
        return uicap_scene_number(scene, value, "delay", &scene->config.delay_ms);
    }
    if (strcmp(command, "clock") == 0) {
        return scene_clock(scene, rest);
    }

    /* A theme before the first frame chooses the look; after it, switching is itself the thing
       worth filming, so it emits a frame like every other command. */
    if (strcmp(command, "theme") == 0) {
        value = uicap_scene_word(&rest);
        if (value == NULL) {
            return uicap_scene_fail(scene, "'theme' needs a name");
        }
        if (!scene->started) {
            snprintf(scene->theme, sizeof scene->theme, "%s", value);
            return 0;
        }
        if ((status = scene_apply_theme(scene, value)) < 0) {
            return status;
        }
        return uicap_scene_emit(scene);
    }

    if (strcmp(command, "pointer") == 0) {
        inkcell_capture_state(scene->host.capture)->pointer = true;
        return scene->started ? uicap_scene_emit(scene) : 0;
    }

    if (strcmp(command, "key") == 0) {
        char *name = uicap_scene_word(&rest);
        char *count_text = uicap_scene_word(&rest);
        if (name == NULL) {
            return uicap_scene_fail(scene, "'key' needs a button");
        }
        const enum inkcell_key key = inkcell_key_from_name(name);
        if (key == INKCELL_KEY_NONE) {
            return uicap_scene_fail(scene, "no button called '%s'", name);
        }
        unsigned count = 1U;
        if (count_text != NULL &&
            (status = uicap_scene_number(scene, count_text, "key count", &count)) < 0) {
            return status;
        }
        if ((status = scene_start(scene)) < 0) {
            return status;
        }
        for (unsigned i = 0U; i < count && status == 0; ++i) {
            status = uicap_scene_press(scene, key);
        }
        return status;
    }

    if (strcmp(command, "frame") == 0) {
        if ((status = scene_start(scene)) < 0 || (status = uicap_scene_emit(scene)) < 0) {
            return status;
        }
        /* Whatever the clock has started since the last frame - a notice that timed out during a
           `hold` is the case - plays out here, the same way a press's does. */
        return uicap_scene_settle(scene);
    }

    if (strcmp(command, "hold") == 0) {
        value = uicap_scene_word(&rest);
        unsigned ms = 0U;
        if (value == NULL) {
            return uicap_scene_fail(scene, "'hold' needs a duration in ms");
        }
        if ((status = uicap_scene_number(scene, value, "hold", &ms)) < 0 ||
            (status = scene_start(scene)) < 0) {
            return status;
        }
        return scene_hold(scene, ms);
    }

    return 1;
}

int uicap_scene_run_line(struct uicap_scene *scene, char *line) {
    scene->line++;
    scene->error[0] = '\0';
    char *rest = line;
    const char *command = uicap_scene_word(&rest);
    if (command == NULL || command[0] == '#') {
        return 0;
    }

    const int generic = scene_run_generic(scene, command, rest);
    if (generic <= 0) {
        return generic;
    }

    const struct uicap_scene_verb *verb = NULL;
    for (size_t i = 0; i < scene->host.verb_count; ++i) {
        if (strcmp(scene->host.verbs[i].name, command) == 0) {
            verb = &scene->host.verbs[i];
            break;
        }
    }
    if (verb == NULL) {
        return uicap_scene_fail(scene, "unknown command '%s'", command);
    }

    int status = 0;
    if ((verb->flags & UICAP_SCENE_SETUP) != 0U) {
        if (scene->started) {
            return uicap_scene_fail(scene, "'%s' has to come before the first frame", command);
        }
    } else if ((status = scene_start(scene)) < 0) {
        return status;
    }
    if ((status = verb->run(scene, rest, scene->host.userdata)) < 0) {
        if (scene->error[0] == '\0') {
            (void)uicap_scene_fail(scene, "'%s' failed: %s", command, strerror(-status));
        }
        return status;
    }
    if ((verb->flags & (UICAP_SCENE_SETUP | UICAP_SCENE_NO_FRAME)) != 0U) {
        return 0;
    }
    if ((status = uicap_scene_emit(scene)) < 0) {
        return status;
    }
    return uicap_scene_settle(scene);
}

#define UICAP_SCENE_LINE_MAX 512U

int uicap_scene_run_file(struct uicap_scene *scene, FILE *file) {
    char line[UICAP_SCENE_LINE_MAX];
    while (fgets(line, (int)sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        const int status = uicap_scene_run_line(scene, line);
        if (status < 0) {
            return status;
        }
    }
    return 0;
}

int uicap_scene_finish(struct uicap_scene *scene) {
    const int status = scene_start(scene);
    if (status < 0) {
        return status;
    }
    /* Once: a second finish must not hand the sink the same frame twice. */
    if (scene->finished) {
        return 0;
    }
    scene->finished = true;
    return scene_flush_delay(scene);
}
