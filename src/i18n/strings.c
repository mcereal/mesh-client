/*
 * The catalog, the locale registry, and the two things a caller can do with them.
 *
 * Every table here comes off include/mesh/i18n/catalog.def, so the ids, the English text and
 * the id names cannot drift apart: they are three expansions of one list. A second language is
 * a second table of the same length plus a row in k_locales, which is the shape src/ui/theme.c
 * uses for themes and for the same reason - adding one should be a table entry, not a hunt.
 *
 * mesh_i18n_validate() is the counterpart to mesh_ui_theme_validate(): the tests run it over
 * every locale in the build, so a translation whose %-specifiers no longer match the English
 * fails there rather than at the snprintf that would read the wrong argument.
 */

#include "mesh/i18n/strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the catalog --------------------------------------------------------------------------- */

static const char *const k_english[MESH_STR_COUNT] = {
#define MESH_STR_ENTRY(id, text) [MESH_STR_##id] = text,
#define MESH_STR_PLURAL_ENTRY(id, one, other)                                                      \
    [MESH_STR_##id##_ONE] = one, [MESH_STR_##id##_OTHER] = other,
#include "mesh/i18n/catalog.def"
#undef MESH_STR_ENTRY
#undef MESH_STR_PLURAL_ENTRY
};

/* The ids as text, for the translation template and for a test failure that has to name the
   entry it is complaining about. */
static const char *const k_id_names[MESH_STR_COUNT] = {
#define MESH_STR_ENTRY(id, text) [MESH_STR_##id] = #id,
#define MESH_STR_PLURAL_ENTRY(id, one, other)                                                      \
    [MESH_STR_##id##_ONE] = #id "_ONE", [MESH_STR_##id##_OTHER] = #id "_OTHER",
#include "mesh/i18n/catalog.def"
#undef MESH_STR_ENTRY
#undef MESH_STR_PLURAL_ENTRY
};

/* ---- locales ------------------------------------------------------------------------------- */

/*
 * "one for exactly 1, other for everything else", which is English, German, Dutch, the
 * Scandinavian languages and most of the rest of Germanic Europe. French counts 0 as one;
 * Polish, Russian and Arabic need more forms than MESH_STR_PLURAL_FORMS has - see docs/i18n.md
 * before adding one of those.
 */
static uint8_t plural_english(uint32_t n) { return (uint8_t)(n == 1U ? 0 : 1); }

/*
 * Every language in the build.
 *
 * English has no table: it *is* the fallback, and giving it one would mean two copies of the
 * same 800 strings to keep in step. A new language is a src/i18n/locale_<id>.c declaring
 * `const char *const mesh_i18n_table_<id>[MESH_STR_COUNT]`, a row here, and the file in
 * CMakeLists.txt - nothing else. docs/i18n.md walks through it.
 */
static const struct mesh_i18n_locale k_locales[] = {
    {
        .id = "en",
        .name = "English",
        .table = NULL,
        .plural_form = plural_english,
    },
};

#define MESH_I18N_LOCALE_COUNT (sizeof k_locales / sizeof k_locales[0])

static const struct mesh_i18n_locale *s_current = &k_locales[0];

size_t mesh_i18n_locale_count(void) { return MESH_I18N_LOCALE_COUNT; }

const struct mesh_i18n_locale *mesh_i18n_locale_at(size_t index) {
    return index < MESH_I18N_LOCALE_COUNT ? &k_locales[index] : NULL;
}

const struct mesh_i18n_locale *mesh_i18n_locale_english(void) { return &k_locales[0]; }

const struct mesh_i18n_locale *mesh_i18n_locale(void) {
    return s_current != NULL ? s_current : &k_locales[0];
}

const struct mesh_i18n_locale *mesh_i18n_locale_by_id(const char *id) {
    if (id == NULL || id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < MESH_I18N_LOCALE_COUNT; ++i) {
        if (strcmp(k_locales[i].id, id) == 0) {
            return &k_locales[i];
        }
    }
    return NULL;
}

bool mesh_i18n_set_locale(const char *id) {
    const struct mesh_i18n_locale *locale = mesh_i18n_locale_by_id(id);
    if (locale == NULL) {
        return false;
    }
    s_current = locale;
    return true;
}

/*
 * "fr_CA.UTF-8" and "fr" both mean French here.
 *
 * POSIX locale names carry a territory and an encoding this client has no use for - the
 * catalog is one text per language, not per territory - so only the part before the first
 * '_', '.' or '@' is matched. A locale that does want to distinguish territories registers
 * itself under the full name and is found by the exact match first.
 */
static const struct mesh_i18n_locale *locale_from_tag(const char *tag) {
    if (tag == NULL || tag[0] == '\0') {
        return NULL;
    }
    const struct mesh_i18n_locale *exact = mesh_i18n_locale_by_id(tag);
    if (exact != NULL) {
        return exact;
    }
    char language[16];
    size_t len = 0U;
    while (tag[len] != '\0' && tag[len] != '_' && tag[len] != '-' && tag[len] != '.' &&
           tag[len] != '@' && len + 1U < sizeof language) {
        language[len] = tag[len];
        len++;
    }
    language[len] = '\0';
    return len > 0U ? mesh_i18n_locale_by_id(language) : NULL;
}

void mesh_i18n_init(void) {
    /* MESHCLIENT_LANG first so the Brick's launch.sh can pin a language without touching the
       system locale, which on a handheld running MinUI is not something a user can set. The
       POSIX variables follow in the order POSIX gives them. */
    static const char *const k_env[] = {"MESHCLIENT_LANG", "LC_ALL", "LC_MESSAGES", "LANG"};
    for (size_t i = 0; i < sizeof k_env / sizeof k_env[0]; ++i) {
        const char *value = getenv(k_env[i]);
        if (value == NULL || value[0] == '\0') {
            continue;
        }
        /* "C" and "POSIX" are the absence of a language, not a request for one. */
        if (strcmp(value, "C") == 0 || strcmp(value, "POSIX") == 0) {
            break;
        }
        const struct mesh_i18n_locale *locale = locale_from_tag(value);
        if (locale != NULL) {
            s_current = locale;
            return;
        }
        /* A language we do not have is still an answer: stop looking rather than letting a
           lower-priority variable name a different one. */
        break;
    }
    s_current = &k_locales[0];
}

/* ---- lookup -------------------------------------------------------------------------------- */

const char *mesh_str_in(const struct mesh_i18n_locale *locale, enum mesh_str_id id) {
    /* Through int rather than compared as an enum: C is free to give an enum with no negative
       members an unsigned type, and "id < 0" on one of those is a warning, not a bounds check. */
    const int index = (int)id;
    if (index < 0 || index >= (int)MESH_STR_COUNT) {
        return "";
    }
    if (locale != NULL && locale->table != NULL && locale->table[index] != NULL) {
        return locale->table[index];
    }
    const char *english = k_english[index];
    return english != NULL ? english : "";
}

const char *mesh_str(enum mesh_str_id id) { return mesh_str_in(mesh_i18n_locale(), id); }

const char *mesh_str_id_name(enum mesh_str_id id) {
    const int index = (int)id;
    return (index >= 0 && index < (int)MESH_STR_COUNT) ? k_id_names[index] : NULL;
}

/* Which of a plural entry's forms `count` takes, clamped so a locale whose rule outgrows
   MESH_STR_PLURAL_FORMS cannot walk off the end of the entry. */
static enum mesh_str_id plural_pick(enum mesh_str_id one_form, uint32_t count) {
    const struct mesh_i18n_locale *locale = mesh_i18n_locale();
    uint8_t form = locale->plural_form != NULL ? locale->plural_form(count) : plural_english(count);
    if (form >= MESH_STR_PLURAL_FORMS) {
        form = MESH_STR_PLURAL_FORMS - 1U;
    }
    const int picked = (int)one_form + (int)form;
    return picked < (int)MESH_STR_COUNT ? (enum mesh_str_id)picked : one_form;
}

const char *mesh_str_plural(enum mesh_str_id one_form, uint32_t count) {
    return mesh_str(plural_pick(one_form, count));
}

/* ---- formatting ---------------------------------------------------------------------------- */

/*
 * The one place in the client that formats a string it did not write.
 *
 * -Wformat=2 objects to a non-literal format because it cannot check the arguments against it,
 * and that objection is real: the check moved from the compiler to mesh_i18n_validate(), which
 * holds every translation to the English entry's specifiers. Suppressing it here rather than at
 * each call site is what keeps that trade in one reviewable place.
 */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
int mesh_str_vformat(char *out, size_t out_len, enum mesh_str_id id, va_list args) {
    if (out == NULL || out_len == 0U) {
        return 0;
    }
    return vsnprintf(out, out_len, mesh_str(id), args);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

int mesh_str_format(char *out, size_t out_len, enum mesh_str_id id, ...) {
    va_list args;
    va_start(args, id);
    const int written = mesh_str_vformat(out, out_len, id, args);
    va_end(args);
    return written;
}

int mesh_str_format_plural(char *out, size_t out_len, enum mesh_str_id one_form, uint32_t count,
                           ...) {
    va_list args;
    va_start(args, count);
    const int written = mesh_str_vformat(out, out_len, plural_pick(one_form, count), args);
    va_end(args);
    return written;
}

/* ---- validation ---------------------------------------------------------------------------- */

/*
 * The %-specifiers a format string carries, in order, as a compact signature.
 *
 * Each conversion contributes its length modifier and its conversion character ("s", "u", "zu",
 * "08x" reduced to "x"); each `*` in a width or precision contributes a "*" of its own, because
 * it consumes an int argument ahead of the value. "%%" contributes nothing. Two strings with
 * the same signature read the same arguments in the same order, which is the whole of what a
 * translation has to preserve.
 *
 * Returns false when the signature does not fit, which for these strings means the entry is
 * doing something no catalog entry should.
 */
static bool format_signature(const char *format, char *out, size_t out_len) {
    size_t used = 0U;
    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';
    for (const char *p = format; *p != '\0'; ++p) {
        if (*p != '%') {
            continue;
        }
        p++;
        if (*p == '%') {
            continue;
        }
        /* Flags, then width, then precision; a '*' in either takes an argument of its own. */
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
            p++;
        }
        for (unsigned part = 0; part < 2U; ++part) {
            if (part == 1U) {
                if (*p != '.') {
                    break;
                }
                p++;
            }
            if (*p == '*') {
                if (used + 2U >= out_len) {
                    return false;
                }
                out[used++] = '*';
                out[used] = '\0';
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    p++;
                }
            }
        }
        /* Length modifiers, which are part of the argument's type and so part of the
           signature: %u and %zu do not read the same argument. */
        char length[3] = {0};
        size_t length_len = 0U;
        while ((*p == 'h' || *p == 'l' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L') &&
               length_len + 1U < sizeof length) {
            length[length_len++] = *p;
            p++;
        }
        if (*p == '\0') {
            return false; /* a trailing '%' with no conversion after it */
        }
        if (used + length_len + 2U >= out_len) {
            return false;
        }
        for (size_t i = 0; i < length_len; ++i) {
            out[used++] = length[i];
        }
        out[used++] = *p;
        out[used] = '\0';
    }
    return true;
}

bool mesh_i18n_validate(const struct mesh_i18n_locale *locale, char *reason, size_t reason_len) {
    if (reason != NULL && reason_len > 0U) {
        reason[0] = '\0';
    }
    if (locale == NULL || locale->id == NULL || locale->id[0] == '\0' || locale->name == NULL) {
        if (reason != NULL) {
            snprintf(reason, reason_len, "%s", "the locale has no id or no name");
        }
        return false;
    }

    for (int id = 0; id < (int)MESH_STR_COUNT; ++id) {
        const char *english = k_english[id];
        if (english == NULL) {
            /* A hole in the table means an id exists with no entry behind it, which can only
               happen if the enum and the catalog have come apart. */
            if (reason != NULL) {
                snprintf(reason, reason_len, "id %d has no English text", id);
            }
            return false;
        }
        char english_signature[64];
        if (!format_signature(english, english_signature, sizeof english_signature)) {
            if (reason != NULL) {
                snprintf(reason, reason_len, "%s: the English text has a format we cannot check",
                         k_id_names[id]);
            }
            return false;
        }
        if (locale->table == NULL || locale->table[id] == NULL) {
            continue; /* not translated; it will read as English */
        }
        char translated_signature[64];
        if (!format_signature(locale->table[id], translated_signature,
                              sizeof translated_signature)) {
            if (reason != NULL) {
                snprintf(reason, reason_len, "%s: the translation has a format we cannot check",
                         k_id_names[id]);
            }
            return false;
        }
        if (strcmp(english_signature, translated_signature) != 0) {
            if (reason != NULL) {
                snprintf(reason, reason_len,
                         "%s: expected the arguments %s, the translation has %s", k_id_names[id],
                         english_signature[0] != '\0' ? english_signature : "(none)",
                         translated_signature[0] != '\0' ? translated_signature : "(none)");
            }
            return false;
        }
    }
    return true;
}
