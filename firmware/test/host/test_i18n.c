/*
 * test_i18n.c — host checks for the UI string catalog: every (lang, id) is a
 * non-empty ASCII string, English is fully populated, and the language picker
 * has an endonym for each language.
 */
#include "i18n.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
} while (0)

int main(void)
{
    for (int id = 0; id < STR_COUNT; ++id) {
        for (int lg = 0; lg < LANG_COUNT; ++lg) {
            const char *s = i18n(lg, id);
            CHECK(s != NULL && s[0] != '\0', "every (lang,id) is non-empty");
            /* ASCII-only so it renders with the 5x7 font. */
            for (const char *p = s; *p; ++p)
                CHECK((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f,
                      "strings are printable ASCII");
        }
    }
    printf("ok  : %d ids x %d langs populated, ASCII-only\n", STR_COUNT, LANG_COUNT);

    for (int lg = 0; lg < LANG_COUNT; ++lg) {
        const char *n = i18n_lang_name(lg);
        CHECK(n && n[0], "each language has an endonym");
    }
    /* Spot-check a couple of translations differ from English. */
    CHECK(strcmp(i18n(LANG_FR, STR_SETTINGS), i18n(LANG_EN, STR_SETTINGS)) != 0,
          "FR SETTINGS differs from EN");
    CHECK(strcmp(i18n(LANG_DE, STR_BACK), i18n(LANG_EN, STR_BACK)) != 0,
          "DE BACK differs from EN");
    /* Out-of-range is safe. */
    CHECK(i18n(-1, -1)[0] == '\0', "out-of-range id -> empty");
    CHECK(i18n(99, STR_MODE) != NULL, "out-of-range lang -> non-null (EN fallback)");

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL I18N TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
