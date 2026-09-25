/*
 * A snackbar's notices - see mesh/ui/toast.h.
 */

#include "mesh/ui/toast.h"

#include <stdio.h>
#include <string.h>

void mesh_ui_toast_init(struct mesh_ui_toast *toast) {
    if (toast != NULL) {
        memset(toast, 0, sizeof *toast);
    }
}

/* Moves the oldest waiting notice up to the snackbar. The caller says whether it is dated. */
static void promote(struct mesh_ui_toast *toast, uint64_t until_ms) {
    snprintf(toast->text, sizeof toast->text, "%s", toast->queue[0]);
    toast->until_ms = until_ms;
    memmove(&toast->queue[0], &toast->queue[1],
            (MESH_UI_TOAST_QUEUE - 1U) * sizeof toast->queue[0]);
    toast->queued--;
    memset(toast->queue[toast->queued], 0, sizeof toast->queue[toast->queued]);
}

/*
 * Takes a notice that cannot be said yet, or says why it need not be.
 *
 * True when the caller has nothing more to do - the notice is queued, or repeats one already on
 * its way. False means the snackbar is free and the caller should put the notice straight up.
 *
 * The repeat test is against what is *showing* and against the newest thing waiting, which is the
 * shape the duplicate actually takes: one event reported twice in a row, rather than the same
 * sentence coming back around after two others. Two identical notices in a row are one notice that
 * stood for eight seconds - a snackbar with a stuck button rather than news.
 */
static bool queue(struct mesh_ui_toast *toast, const char *text) {
    if (toast->text[0] == '\0') {
        return false; /* nothing is up; say it now */
    }
    const char *newest = toast->queued > 0U ? toast->queue[toast->queued - 1U] : toast->text;
    if (strcmp(newest, text) == 0) {
        return true;
    }
    if (toast->queued >= MESH_UI_TOAST_QUEUE) {
        /* Drop the oldest waiting one and close the gap - see the field for why it is that end. */
        memmove(&toast->queue[0], &toast->queue[1],
                (MESH_UI_TOAST_QUEUE - 1U) * sizeof toast->queue[0]);
        toast->queued = MESH_UI_TOAST_QUEUE - 1U;
    }
    snprintf(toast->queue[toast->queued++], MESH_UI_TOAST_TEXT_MAX, "%s", text);
    return true;
}

void mesh_ui_toast_set(struct mesh_ui_toast *toast, uint64_t now_ms, const char *text) {
    if (toast == NULL) {
        return;
    }
    if (text == NULL || text[0] == '\0') {
        mesh_ui_toast_init(toast);
        return;
    }
    snprintf(toast->text, sizeof toast->text, "%s", text);
    toast->until_ms = now_ms + MESH_UI_TOAST_STAND_MS;
}

void mesh_ui_toast_raise(struct mesh_ui_toast *toast, const char *text) {
    if (toast == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    snprintf(toast->text, sizeof toast->text, "%s", text);
    toast->until_ms = 0U;
}

void mesh_ui_toast_post(struct mesh_ui_toast *toast, uint64_t now_ms, const char *text) {
    if (toast == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    if (queue(toast, text)) {
        return;
    }
    snprintf(toast->text, sizeof toast->text, "%s", text);
    toast->until_ms = now_ms + MESH_UI_TOAST_STAND_MS;
}

/*
 * Only the one showing has been seen. Cleared along with it, the queue would outlive the snackbar
 * and sit unwalked - the tick only promotes while something is showing - until a later notice went
 * straight up ahead of it and the older ones followed, out of order. The one promoted here is
 * undated, exactly as a notice a press raises is, and is dated before anything is drawn.
 */
bool mesh_ui_toast_dismiss(struct mesh_ui_toast *toast) {
    if (toast == NULL || toast->text[0] == '\0') {
        return false;
    }
    if (toast->queued > 0U) {
        promote(toast, 0U);
    } else {
        toast->text[0] = '\0';
        toast->until_ms = 0U;
    }
    return true;
}

void mesh_ui_toast_date(struct mesh_ui_toast *toast, uint64_t now_ms) {
    if (toast == NULL || toast->text[0] == '\0' || toast->until_ms != 0U) {
        return;
    }
    toast->until_ms = now_ms + MESH_UI_TOAST_STAND_MS;
}

bool mesh_ui_toast_tick(struct mesh_ui_toast *toast, uint64_t now_ms) {
    if (toast == NULL || toast->text[0] == '\0' || now_ms < toast->until_ms) {
        return false;
    }
    if (toast->queued > 0U) {
        /* The next one takes the snackbar, dated from this tick rather than from whenever it was
           raised: it is starting to stand now, and a deadline the backend has not seen is how it
           tells one notice from the next. */
        promote(toast, now_ms + MESH_UI_TOAST_STAND_MS);
        return true;
    }
    toast->text[0] = '\0';
    toast->until_ms = 0U;
    return true;
}
