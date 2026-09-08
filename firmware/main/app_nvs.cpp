/*
 * app_nvs.cpp — load/save persisted user configuration.
 * Matter's own fabric/Thread credentials live in the stack's NVS, separate from
 * this "thermo_cfg" namespace.
 */
#include "app_priv.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "app_nvs"
#define NS  "thermo_cfg"

void app_config_defaults(app_config_t *cfg)
{
    cfg->mode          = THERMO_MODE_OFF;
    cfg->heat_set_c100 = CONFIG_THERMO_DEFAULT_HEAT_SET_C10 * 10;
    cfg->cool_set_c100 = CONFIG_THERMO_DEFAULT_COOL_SET_C10 * 10;
    cfg->deadband_c10  = CONFIG_THERMO_DEFAULT_DEADBAND_C10;
    cfg->offset_c100   = 0;
#if CONFIG_THERMO_DEFAULT_UNITS_FAHRENHEIT
    cfg->fahrenheit    = true;
#else
    cfg->fahrenheit    = false;
#endif
#if CONFIG_THERMO_NTC_TYPE2
    cfg->ntc_type      = NTC_TYPE_2;
#else
    cfg->ntc_type      = NTC_TYPE_3;
#endif
    cfg->min_off_s     = CONFIG_THERMO_MIN_OFF_S;
    cfg->min_on_s      = CONFIG_THERMO_MIN_ON_S;
    cfg->hp_reversing  = THERMO_HP_NONE;
    cfg->brightness    = 200;
    cfg->fan_speed     = THERMO_FAN_AUTO;
#if CONFIG_THERMO_OCC_SOURCE_SENSOR
    cfg->occ_source    = OCC_SRC_SENSOR;
#else
    cfg->occ_source    = OCC_SRC_MANUAL;
#endif
    cfg->occ_manual_home = true;    /* default Home */
    cfg->unocc_heat_c100 = CONFIG_THERMO_DEFAULT_UNOCC_HEAT_SET_C10 * 10;
    cfg->unocc_cool_c100 = CONFIG_THERMO_DEFAULT_UNOCC_COOL_SET_C10 * 10;
}

static void get_i32(nvs_handle_t h, const char *k, int *v)
{
    int32_t tmp;
    if (nvs_get_i32(h, k, &tmp) == ESP_OK) *v = (int)tmp;
}
static void get_u8b(nvs_handle_t h, const char *k, bool *v)
{
    uint8_t tmp;
    if (nvs_get_u8(h, k, &tmp) == ESP_OK) *v = tmp != 0;
}

int app_nvs_load(app_config_t *cfg)
{
    app_config_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved config; using defaults");
        return ESP_OK;
    }
    get_i32(h, "mode",     &cfg->mode);
    get_i32(h, "heat",     &cfg->heat_set_c100);
    get_i32(h, "cool",     &cfg->cool_set_c100);
    get_i32(h, "deadband", &cfg->deadband_c10);
    get_i32(h, "offset",   &cfg->offset_c100);
    get_u8b(h, "units_f",  &cfg->fahrenheit);
    get_i32(h, "ntc",      &cfg->ntc_type);
    get_i32(h, "min_off",  &cfg->min_off_s);
    get_i32(h, "min_on",   &cfg->min_on_s);
    get_i32(h, "hp",       &cfg->hp_reversing);
    get_i32(h, "bright",   &cfg->brightness);
    get_i32(h, "fan",      &cfg->fan_speed);
    get_i32(h, "occSrc",   &cfg->occ_source);
    get_u8b(h, "occHome",  &cfg->occ_manual_home);
    get_i32(h, "uHeat",    &cfg->unocc_heat_c100);
    get_i32(h, "uCool",    &cfg->unocc_cool_c100);
    nvs_close(h);
    ESP_LOGI(TAG, "config loaded");
    return ESP_OK;
}

int app_nvs_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_i32(h, "mode",     cfg->mode);
    nvs_set_i32(h, "heat",     cfg->heat_set_c100);
    nvs_set_i32(h, "cool",     cfg->cool_set_c100);
    nvs_set_i32(h, "deadband", cfg->deadband_c10);
    nvs_set_i32(h, "offset",   cfg->offset_c100);
    nvs_set_u8 (h, "units_f",  cfg->fahrenheit ? 1 : 0);
    nvs_set_i32(h, "ntc",      cfg->ntc_type);
    nvs_set_i32(h, "min_off",  cfg->min_off_s);
    nvs_set_i32(h, "min_on",   cfg->min_on_s);
    nvs_set_i32(h, "hp",       cfg->hp_reversing);
    nvs_set_i32(h, "bright",   cfg->brightness);
    nvs_set_i32(h, "fan",      cfg->fan_speed);
    nvs_set_i32(h, "occSrc",   cfg->occ_source);
    nvs_set_u8 (h, "occHome",  cfg->occ_manual_home ? 1 : 0);
    nvs_set_i32(h, "uHeat",    cfg->unocc_heat_c100);
    nvs_set_i32(h, "uCool",    cfg->unocc_cool_c100);

    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "config saved");
    return err;
}
