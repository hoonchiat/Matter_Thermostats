/*
 * thermistor.c — ESP-IDF v5 ADC oneshot + calibration driver for the NTC.
 * Sampling: median-of-N raw -> calibrated mV -> R -> T (LUT) -> EMA -> +offset.
 */
#include "thermistor.h"

#include <string.h>
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#define TAG "thermistor"
#define MAX_MEDIAN_N 15
#define FAULT_MARGIN_MV 30      /* within this of 0 or Vref => open/short */

static struct {
    thermistor_config_t     cfg;
    adc_oneshot_unit_handle_t adc;
    adc_cali_handle_t       cali;
    adc_channel_t           chan;
    bool                    have_cali;
    bool                    ema_valid;
    float                   ema_c;
} s;

static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

int thermistor_init(const thermistor_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(&s, 0, sizeof(s));
    s.cfg = *cfg;
    if (s.cfg.median_n < 1)  s.cfg.median_n = 1;
    if (s.cfg.median_n > MAX_MEDIAN_N) s.cfg.median_n = MAX_MEDIAN_N;
    if (s.cfg.ema_alpha_pct < 1)   s.cfg.ema_alpha_pct = 1;
    if (s.cfg.ema_alpha_pct > 100) s.cfg.ema_alpha_pct = 100;

    adc_unit_t unit;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(s.cfg.adc_gpio, &unit, &channel);
    if (err != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO %d is not an ADC1 channel", s.cfg.adc_gpio);
        return ESP_ERR_INVALID_ARG;
    }
    s.chan = channel;

    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
    err = adc_oneshot_new_unit(&init_cfg, &s.adc);
    if (err != ESP_OK) return err;

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s.adc, s.chan, &chan_cfg);
    if (err != ESP_OK) return err;

    /* Curve-fitting calibration (supported on ESP32-C6). */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = s.chan,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s.cali) == ESP_OK) {
        s.have_cali = true;
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable; using ratiometric estimate");
    }

    ESP_LOGI(TAG, "init: gpio=%d chan=%d rfix=%d vref=%d type=%d",
             s.cfg.adc_gpio, s.chan, s.cfg.r_fix_ohm, s.cfg.vref_mv, s.cfg.type);
    return ESP_OK;
}

static int read_mv_once(void)
{
    int raw = 0;
    if (adc_oneshot_read(s.adc, s.chan, &raw) != ESP_OK) return -1;
    if (s.have_cali) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(s.cali, raw, &mv) == ESP_OK) return mv;
    }
    /* Fallback: assume 12-bit full-scale maps to Vref. */
    return (int)((int64_t)raw * s.cfg.vref_mv / 4095);
}

int thermistor_read(float *temp_c, bool *fault)
{
    int mv_samples[MAX_MEDIAN_N];
    int n = 0;
    for (int i = 0; i < s.cfg.median_n; ++i) {
        int mv = read_mv_once();
        if (mv >= 0) mv_samples[n++] = mv;
    }
    if (n == 0) {
        if (fault) *fault = true;
        return ESP_FAIL;
    }
    qsort(mv_samples, n, sizeof(int), cmp_int);
    int mv = mv_samples[n / 2];

    /* Open (V ~ Vref) or short (V ~ 0) detection. */
    bool is_fault = (mv < FAULT_MARGIN_MV) || (mv > s.cfg.vref_mv - FAULT_MARGIN_MV);
    if (fault) *fault = is_fault;
    if (is_fault) {
        s.ema_valid = false;                 /* drop stale smoothing on fault */
        return ESP_OK;
    }

    float sample_c = ntc_temp_c_from_mv(s.cfg.type, (float)mv,
                                        (float)s.cfg.vref_mv,
                                        (float)s.cfg.r_fix_ohm);

    float alpha = s.cfg.ema_alpha_pct / 100.0f;
    if (!s.ema_valid) { s.ema_c = sample_c; s.ema_valid = true; }
    else              { s.ema_c = alpha * sample_c + (1.0f - alpha) * s.ema_c; }

    if (temp_c) *temp_c = s.ema_c + (s.cfg.offset_c100 / 100.0f);
    return ESP_OK;
}

void thermistor_set_offset_c100(int offset_c100) { s.cfg.offset_c100 = offset_c100; }
void thermistor_set_type(ntc_type_t type)        { s.cfg.type = type; s.ema_valid = false; }
