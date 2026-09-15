/*
 * i18n.c — the UI string catalog. One row per message id (msg_t), one column
 * per language (lang_t). Keep the columns in lang_t order: EN, FR, ES, DE.
 */
#include "i18n.h"
#include <stddef.h>

/* [msg][lang] */
static const char *const TBL[STR_COUNT][LANG_COUNT] = {
    /*                     EN               FR                ES                 DE               */
    [STR_SETTINGS]     = {"SETTINGS",      "REGLAGES",       "AJUSTES",         "EINSTELLUNGEN"},
    [STR_MODE]         = {"MODE",          "MODE",           "MODO",            "MODUS"},
    [STR_FAN]          = {"FAN",           "VENTILO",        "VENTILADOR",      "LUEFTER"},
    [STR_PRESENCE]     = {"PRESENCE",      "PRESENCE",       "PRESENCIA",       "ANWESENHEIT"},
    [STR_SOURCE]       = {"SOURCE",        "SOURCE",         "FUENTE",          "QUELLE"},
    [STR_UNITS]        = {"UNITS",         "UNITES",         "UNIDADES",        "EINHEIT"},
    [STR_LANGUAGE]     = {"LANGUAGE",      "LANGUE",         "IDIOMA",          "SPRACHE"},
    [STR_MATTER_CODE]  = {"MATTER CODE",   "CODE MATTER",    "CODIGO MATTER",   "MATTER-CODE"},
    [STR_BACK]         = {"BACK",          "RETOUR",         "ATRAS",           "ZURUECK"},

    [STR_M_OFF]        = {"OFF",           "ARRET",          "APAG",            "AUS"},
    [STR_M_HEAT]       = {"HEAT",          "CHAUF",          "CALOR",           "HEIZ"},
    [STR_M_COOL]       = {"COOL",          "CLIM",           "FRIO",            "KUEHL"},
    [STR_M_AUTO]       = {"AUTO",          "AUTO",           "AUTO",            "AUTO"},
    [STR_M_FAN]        = {"FAN",           "VENT",           "VENT",            "LUFT"},

    [STR_F_AUTO]       = {"AUTO",          "AUTO",           "AUTO",            "AUTO"},
    [STR_F_LOW]        = {"LOW",           "BAS",            "BAJA",            "NIED"},
    [STR_F_MED]        = {"MED",           "MOY",            "MED",             "MITT"},
    [STR_F_HIGH]       = {"HIGH",          "HAUT",           "ALTA",            "HOCH"},

    [STR_HOME]         = {"HOME",          "PRESENT",        "PRESENTE",        "ANWESEND"},
    [STR_AWAY]         = {"AWAY",          "ABSENT",         "AUSENTE",         "ABWESEND"},
    [STR_MANUAL]       = {"MANUAL",        "MANUEL",         "MANUAL",          "MANUELL"},
    [STR_SENSOR]       = {"SENSOR",        "CAPTEUR",        "SENSOR",          "SENSOR"},

    [STR_HEATING]      = {"HEATING",       "CHAUFFE",        "CALENTANDO",      "HEIZT"},
    [STR_COOLING]      = {"COOLING",       "REFROIDIT",      "ENFRIANDO",       "KUEHLT"},
    [STR_IDLE]         = {"IDLE",          "REPOS",          "REPOSO",          "BEREIT"},
    [STR_FAN_ON]       = {"FAN ON",        "VENTILE",        "VENTILANDO",      "LUEFTET"},

    [STR_SET]          = {"SET",           "REG",            "AJU",             "SOLL"},
    [STR_PAIR_TITLE]   = {"PAIR THERMOSTAT","APPAIRAGE",     "EMPAREJAR",       "KOPPLUNG"},
    [STR_CODE]         = {"CODE:",         "CODE:",          "CODIGO:",         "CODE:"},
    [STR_NEEDS_BR]     = {"NEEDS THREAD BR","ROUTEUR THREAD","SE NECESITA BR",  "THREAD-ROUTER"},
    [STR_SCAN]         = {"SCAN QR OR ENTER","SCANNER LE QR","ESCANEA EL QR",   "QR SCANNEN"},

    [STR_FAULT1]       = {"! SENSOR FAULT","! DEFAUT CAPTEUR","! FALLO SENSOR", "! SENSORFEHLER"},
    [STR_FAULT2]       = {"CHECK ROOM SENSOR","VERIFIER CAPTEUR","REVISA EL SENSOR","SENSOR PRUEFEN"},
    [STR_FAULT3]       = {"OUTPUTS DISABLED","SORTIES COUPEES","SALIDAS APAGADAS","AUSGAENGE AUS"},

    [STR_BOOT_OPTS]    = {"BOOT OPTIONS",  "OPTIONS BOOT",   "OPCIONES BOOT",   "BOOT-OPTIONEN"},
    [STR_PAIRING]      = {"PAIRING",       "APPAIRAGE",      "EMPAREJAR",       "KOPPLUNG"},
    [STR_FACTORY_RESET]= {"FACTORY RESET", "REINIT. USINE",  "REINICIO FABR.",  "WERKSRESET"},
    [STR_CANCEL]       = {"CANCEL",        "ANNULER",        "CANCELAR",        "ABBRECHEN"},
    [STR_HINT]         = {"TURN:SEL  PRESS:OK","TOURNER:SEL  APP:OK","GIRA:SEL  PULSA:OK","DREHEN:WAHL DRUCK:OK"},
    [STR_IDENTIFY]     = {"IDENTIFY",      "IDENTIFIER",     "IDENTIFICAR",     "IDENTIFIZIEREN"},
};

static const char *const LANG_NAME[LANG_COUNT] = {
    "ENGLISH", "FRANCAIS", "ESPANOL", "DEUTSCH",
};

const char *i18n(int lang, int id)
{
    if (id < 0 || id >= STR_COUNT) return "";
    if (lang < 0 || lang >= LANG_COUNT) lang = LANG_EN;
    const char *s = TBL[id][lang];
    if (!s) s = TBL[id][LANG_EN];
    return s ? s : "";
}

const char *i18n_lang_name(int lang)
{
    if (lang < 0 || lang >= LANG_COUNT) lang = LANG_EN;
    return LANG_NAME[lang];
}
