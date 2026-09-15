/*
 * i18n — tiny UI string catalog for the thermostat's on-device text.
 *
 * Languages: English, French, Spanish, German. Strings are ASCII so they render
 * with the existing 5x7 OLED font and the current layout unchanged. French and
 * Spanish uppercase drop diacritics (conventional on small displays); German
 * umlauts are transliterated (ae/oe/ue, ss) which is orthographically valid.
 *
 * Pure C, no ESP dependency — compiles on host for the UI render tests.
 */
#ifndef I18N_H
#define I18N_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LANG_EN = 0,
    LANG_FR,
    LANG_ES,
    LANG_DE,
    LANG_COUNT,
} lang_t;

typedef enum {
    STR_SETTINGS = 0,
    STR_MODE,
    STR_FAN,
    STR_PRESENCE,
    STR_SOURCE,
    STR_UNITS,
    STR_LANGUAGE,
    STR_MATTER_CODE,
    STR_BACK,
    STR_M_OFF,
    STR_M_HEAT,
    STR_M_COOL,
    STR_M_AUTO,
    STR_M_FAN,
    STR_F_AUTO,
    STR_F_LOW,
    STR_F_MED,
    STR_F_HIGH,
    STR_HOME,
    STR_AWAY,
    STR_MANUAL,
    STR_SENSOR,
    STR_HEATING,
    STR_COOLING,
    STR_IDLE,
    STR_FAN_ON,
    STR_SET,
    STR_PAIR_TITLE,
    STR_CODE,
    STR_NEEDS_BR,
    STR_SCAN,
    STR_FAULT1,
    STR_FAULT2,
    STR_FAULT3,
    STR_BOOT_OPTS,
    STR_PAIRING,
    STR_FACTORY_RESET,
    STR_CANCEL,
    STR_HINT,
    STR_IDENTIFY,
    STR_COUNT,
} msg_t;

/* Return the UTF-8 (ASCII) string for (lang, id). Out-of-range falls back to
 * English / empty, never NULL. */
const char *i18n(int lang, int id);

/* Endonym for the language picker row (ENGLISH / FRANCAIS / ESPANOL / DEUTSCH). */
const char *i18n_lang_name(int lang);

#ifdef __cplusplus
}
#endif

#endif /* I18N_H */
